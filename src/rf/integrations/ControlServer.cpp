#include "rf/integrations/ControlServer.h"

#include <sddl.h>

#include <algorithm>
#include <memory>

#include "rf/core/Log.h"
#include "rf/integrations/Protocol.h"

namespace rf::integrations {
namespace {

constexpr std::size_t kMaxInstances = 32;
constexpr DWORD kWriteTimeoutMs = 200;

std::wstring CurrentUserSid() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return {};

    DWORD size = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::uint8_t> buffer(size);
    std::wstring sid;
    if (::GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        LPWSTR text = nullptr;
        if (::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,
                                     &text)) {
            sid = text;
            ::LocalFree(text);
        }
    }
    ::CloseHandle(token);
    return sid;
}

}

struct ControlServer::Instance {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    OVERLAPPED io{};
    bool connected = false;
    std::uint64_t client = 0;
    std::uint32_t pid = 0;
    char buffer[kMaxMessageBytes]{};

    Instance() { io.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~Instance() {
        if (pipe != INVALID_HANDLE_VALUE) ::CloseHandle(pipe);
        if (io.hEvent) ::CloseHandle(io.hEvent);
    }
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;
};

ControlServer::ControlServer() = default;

ControlServer::~ControlServer() { Stop(); }

Status ControlServer::Start(const std::wstring& pipe_name) {
    if (running_) return Status::Ok();

    const std::wstring sid = CurrentUserSid();
    if (sid.empty()) return Status::Fail("cannot read the current user's SID");
    pipe_name_ = pipe_name;
    security_ = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;" + sid + L")S:(ML;;NW;;;ME)";

    wake_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake_) return Status::Fail(HRESULT_FROM_WIN32(::GetLastError()), "CreateEvent");

    if (auto s = Listen(true); !s.ok()) {
        ::CloseHandle(wake_);
        wake_ = nullptr;
        return s;
    }

    running_ = true;
    thread_ = std::thread([this] { Loop(); });
    return Status::Ok();
}

void ControlServer::Stop() {
    if (running_.exchange(false)) {
        ::SetEvent(wake_);
        if (thread_.joinable()) thread_.join();
    }
    for (auto& instance : instances_) Close(*instance);
    instances_.clear();
    if (wake_) ::CloseHandle(wake_);
    wake_ = nullptr;
}

std::vector<ControlServer::Incoming> ControlServer::Take() {
    std::scoped_lock lock(inbox_mutex_);
    return std::exchange(inbox_, {});
}

void ControlServer::Send(std::uint64_t client, std::string line) {
    {
        std::scoped_lock lock(outbox_mutex_);
        outbox_.push_back({client, std::move(line)});
    }
    if (wake_) ::SetEvent(wake_);
}

Status ControlServer::Listen(bool first) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(security_.c_str(), SDDL_REVISION_1,
                                                                &descriptor, nullptr))
        return Status::Fail(HRESULT_FROM_WIN32(::GetLastError()), "pipe security descriptor");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};

    auto instance = std::make_unique<Instance>();
    instance->pipe = ::CreateNamedPipeW(
        pipe_name_.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, kMaxMessageBytes, kMaxMessageBytes, 0, &attributes);
    const DWORD create_error = ::GetLastError();
    ::LocalFree(descriptor);
    if (instance->pipe == INVALID_HANDLE_VALUE)
        return Status::Fail(HRESULT_FROM_WIN32(create_error), "CreateNamedPipe");

    if (!::ConnectNamedPipe(instance->pipe, &instance->io)) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_PIPE_CONNECTED)
            ::SetEvent(instance->io.hEvent);
        else if (error != ERROR_IO_PENDING)
            return Status::Fail(HRESULT_FROM_WIN32(error), "ConnectNamedPipe");
    }
    instances_.push_back(std::move(instance));
    return Status::Ok();
}

void ControlServer::Loop() {
    ::SetThreadDescription(::GetCurrentThread(), L"rf-sdk-pipe");

    while (running_) {
        std::vector<HANDLE> handles{wake_};
        for (const auto& instance : instances_) handles.push_back(instance->io.hEvent);

        const DWORD signaled = ::WaitForMultipleObjects(static_cast<DWORD>(handles.size()),
                                                        handles.data(), FALSE, INFINITE);
        if (!running_) break;

        if (signaled == WAIT_OBJECT_0) {
            std::vector<Outgoing> outgoing;
            {
                std::scoped_lock lock(outbox_mutex_);
                outgoing.swap(outbox_);
            }
            for (const Outgoing& message : outgoing) {
                for (auto& instance : instances_) {
                    if (!instance->connected || instance->client != message.client) continue;
                    if (!Write(*instance, message.line)) Close(*instance);
                }
            }
        } else if (signaled > WAIT_OBJECT_0 && signaled < WAIT_OBJECT_0 + handles.size()) {
            OnSignaled(*instances_[signaled - WAIT_OBJECT_0 - 1]);
        } else {
            RF_WARN("SDK pipe wait failed ({}) - stopping the SDK server", ::GetLastError());
            break;
        }

        std::erase_if(instances_, [](const auto& i) { return i->pipe == INVALID_HANDLE_VALUE; });

        const bool listening = std::any_of(instances_.begin(), instances_.end(),
                                           [](const auto& i) { return !i->connected; });
        if (!listening && instances_.size() < kMaxInstances) {
            if (auto s = Listen(false); !s.ok()) {
                RF_WARN("SDK pipe: {}", s.str());
                ::Sleep(1000);
            }
        }
    }
}

void ControlServer::OnSignaled(Instance& instance) {
    DWORD bytes = 0;
    const bool done = ::GetOverlappedResult(instance.pipe, &instance.io, &bytes, FALSE) != FALSE;

    if (!instance.connected) {
        if (!done) {
            Close(instance);
            return;
        }
        instance.connected = true;
        instance.client = next_client_++;
        ULONG pid = 0;
        ::GetNamedPipeClientProcessId(instance.pipe, &pid);
        instance.pid = pid;
        Emit({Incoming::Kind::Connected, instance.client, instance.pid, {}});
        IssueRead(instance);
        return;
    }

    if (!done) {
        Close(instance);
        return;
    }
    Deliver(instance, bytes);
    IssueRead(instance);
}

bool ControlServer::IssueRead(Instance& instance) {
    if (::ReadFile(instance.pipe, instance.buffer, sizeof(instance.buffer), nullptr,
                   &instance.io) ||
        ::GetLastError() == ERROR_IO_PENDING)
        return true;
    Close(instance);
    return false;
}

void ControlServer::Deliver(Instance& instance, std::size_t bytes) {
    const std::string_view message(instance.buffer, bytes);
    std::size_t begin = 0;
    while (begin < message.size()) {
        std::size_t end = message.find('\n', begin);
        if (end == std::string_view::npos) end = message.size();
        std::string_view line = message.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty())
            Emit({Incoming::Kind::Line, instance.client, instance.pid, std::string(line)});
        begin = end + 1;
    }
}

bool ControlServer::Write(Instance& instance, const std::string& line) {
    const std::string data = line + "\n";
    OVERLAPPED io{};
    io.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!io.hEvent) return false;

    DWORD written = 0;
    bool ok = ::WriteFile(instance.pipe, data.data(), static_cast<DWORD>(data.size()), nullptr,
                          &io) ||
              ::GetLastError() == ERROR_IO_PENDING;
    if (ok && !::GetOverlappedResultEx(instance.pipe, &io, &written, kWriteTimeoutMs, FALSE)) {
        ::CancelIoEx(instance.pipe, &io);
        ::GetOverlappedResult(instance.pipe, &io, &written, TRUE);
        ok = false;
    }
    ::CloseHandle(io.hEvent);
    return ok && written == data.size();
}

void ControlServer::Close(Instance& instance) {
    if (instance.pipe == INVALID_HANDLE_VALUE) return;

    DWORD ignored = 0;
    ::CancelIoEx(instance.pipe, nullptr);
    ::GetOverlappedResult(instance.pipe, &instance.io, &ignored, TRUE);
    ::DisconnectNamedPipe(instance.pipe);
    ::CloseHandle(instance.pipe);
    instance.pipe = INVALID_HANDLE_VALUE;

    if (instance.connected) Emit({Incoming::Kind::Closed, instance.client, instance.pid, {}});
    instance.connected = false;
}

void ControlServer::Emit(Incoming incoming) {
    std::scoped_lock lock(inbox_mutex_);
    inbox_.push_back(std::move(incoming));
}

}
