#include "rf/gallery/Gallery.h"

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include <algorithm>
#include <chrono>
#include <format>

#include <wrl/client.h>

#include "rf/core/Log.h"
#include "rf/core/Strings.h"

#pragma comment(lib, "propsys.lib")

using Microsoft::WRL::ComPtr;

namespace rf {
namespace {

constexpr DWORD kAllStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD kFirstVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD kMediaSource = static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE);

std::string FormatDuration(double seconds) {
    const int total = static_cast<int>(seconds + 0.5);
    return std::format("{:02}:{:02}", total / 60, total % 60);
}

std::string FormatDate(std::int64_t filetime_ticks) {
    FILETIME ft{};
    ft.dwLowDateTime = static_cast<DWORD>(filetime_ticks & 0xFFFFFFFF);
    ft.dwHighDateTime = static_cast<DWORD>(filetime_ticks >> 32);

    FILETIME local{};
    SYSTEMTIME st{};
    ::FileTimeToLocalFileTime(&ft, &local);
    ::FileTimeToSystemTime(&local, &st);
    return std::format("{:02}.{:02}.{:04}", st.wDay, st.wMonth, st.wYear);
}

std::int64_t WriteTimeTicks(const std::filesystem::path& file) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<std::int64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
           data.ftLastWriteTime.dwLowDateTime;
}

}

Status ExtractThumbnail(const std::filesystem::path& file, std::uint32_t max_edge,
                        std::vector<std::uint8_t>& out_bgra, std::uint32_t& out_w,
                        std::uint32_t& out_h, double& out_duration_seconds) {
    ComPtr<IMFAttributes> attrs;
    RF_HR(MFCreateAttributes(&attrs, 2));

    RF_HR(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE));
    RF_HR(attrs->SetUINT32(MF_LOW_LATENCY, TRUE));

    ComPtr<IMFSourceReader> reader;
    RF_HR(MFCreateSourceReaderFromURL(file.c_str(), attrs.Get(), &reader));

    RF_HR(reader->SetStreamSelection(kAllStreams, FALSE));
    RF_HR(reader->SetStreamSelection(kFirstVideoStream, TRUE));

    ComPtr<IMFMediaType> type;
    RF_HR(MFCreateMediaType(&type));
    RF_HR(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    RF_HR(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));
    RF_HR(reader->SetCurrentMediaType(kFirstVideoStream, nullptr, type.Get()));

    ComPtr<IMFMediaType> actual;
    RF_HR(reader->GetCurrentMediaType(kFirstVideoStream, &actual));
    UINT32 width = 0, height = 0;
    RF_HR(MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &width, &height));
    if (width == 0 || height == 0) return Status::Fail("thumbnail source has no frame size");

    PROPVARIANT duration{};
    ::PropVariantInit(&duration);
    if (SUCCEEDED(reader->GetPresentationAttribute(kMediaSource, MF_PD_DURATION, &duration)) &&
        duration.vt == VT_UI8) {
        out_duration_seconds = static_cast<double>(duration.uhVal.QuadPart) / 10'000'000.0;
    }
    ::PropVariantClear(&duration);

    if (out_duration_seconds > 2.0) {
        PROPVARIANT pos{};
        ::InitPropVariantFromInt64(static_cast<LONGLONG>(1.0 * 10'000'000), &pos);
        reader->SetCurrentPosition(GUID_NULL, pos);
        ::PropVariantClear(&pos);
    }

    ComPtr<IMFSample> sample;
    DWORD stream_flags = 0;
    for (int attempt = 0; attempt < 32 && !sample; ++attempt) {
        RF_HR(reader->ReadSample(kFirstVideoStream, 0, nullptr, &stream_flags,
                                 nullptr, &sample));
        if (stream_flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    if (!sample) return Status::Fail("no decodable video frame");

    ComPtr<IMFMediaBuffer> buffer;
    RF_HR(sample->ConvertToContiguousBuffer(&buffer));

    BYTE* data = nullptr;
    DWORD length = 0;
    RF_HR(buffer->Lock(&data, nullptr, &length));

    const std::uint32_t scale =
        std::max(1u, std::max(width, height) / std::max(1u, max_edge));
    out_w = width / scale;
    out_h = height / scale;
    out_bgra.assign(static_cast<std::size_t>(out_w) * out_h * 4, 0);

    const std::size_t src_pitch = static_cast<std::size_t>(width) * 4;
    for (std::uint32_t y = 0; y < out_h; ++y) {
        const BYTE* src_row = data + (static_cast<std::size_t>(y) * scale) * src_pitch;
        std::uint8_t* dst_row = out_bgra.data() + static_cast<std::size_t>(y) * out_w * 4;
        for (std::uint32_t x = 0; x < out_w; ++x) {
            const BYTE* px = src_row + static_cast<std::size_t>(x) * scale * 4;
            dst_row[x * 4 + 0] = px[2];
            dst_row[x * 4 + 1] = px[1];
            dst_row[x * 4 + 2] = px[0];
            dst_row[x * 4 + 3] = 255;
        }
    }
    buffer->Unlock();
    return Status::Ok();
}

Gallery::~Gallery() { Stop(); }

void Gallery::Start(const std::filesystem::path& dir) {
    if (running_) return;
    dir_ = dir;
    running_ = true;
    rescan_ = true;
    thread_ = std::thread([this] { Worker(); });
}

void Gallery::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void Gallery::Refresh() { rescan_ = true; }

void Gallery::SetDirectory(const std::filesystem::path& dir) {
    std::scoped_lock lock(mutex_);
    if (dir_ == dir) return;
    dir_ = dir;
    items_.clear();
    rescan_ = true;
}

std::vector<GalleryItem> Gallery::Snapshot() const {
    std::scoped_lock lock(mutex_);
    return items_;
}

void Gallery::MarkUploaded(const std::filesystem::path& path) {
    std::scoped_lock lock(mutex_);
    for (auto& item : items_) {
        if (item.path == path) {
            item.thumb_uploaded = true;
            item.thumbnail.clear();
            item.thumbnail.shrink_to_fit();
            return;
        }
    }
}

void Gallery::Remove(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) RF_WARN("could not delete {}: {}", path.string(), ec.message());
    rescan_ = true;
}

void Gallery::Worker() {
    ::SetThreadDescription(::GetCurrentThread(), L"rf-gallery");
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);

    while (running_.load(std::memory_order_relaxed)) {
        if (rescan_.exchange(false)) {
            scanning_ = true;

            std::vector<GalleryItem> found;
            std::uint64_t total = 0;
            std::error_code ec;

            for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                const auto ext = entry.path().extension().wstring();
                if (ext != L".mp4" && ext != L".mkv") continue;

                GalleryItem item;
                item.path = entry.path();
                item.display_name = ToUtf8(entry.path().filename().wstring());
                item.size_bytes = entry.file_size(ec);
                item.sort_key = WriteTimeTicks(entry.path());
                item.date_label = FormatDate(item.sort_key);
                item.duration_label = "--:--";
                total += item.size_bytes;
                found.push_back(std::move(item));
            }

            std::sort(found.begin(), found.end(),
                      [](const GalleryItem& a, const GalleryItem& b) {
                          return a.sort_key > b.sort_key;
                      });

            {
                std::scoped_lock lock(mutex_);
                for (auto& item : found) {
                    for (const auto& old : items_) {
                        if (old.path != item.path) continue;
                        item.thumbnail = old.thumbnail;
                        item.thumb_width = old.thumb_width;
                        item.thumb_height = old.thumb_height;
                        item.thumb_ready = old.thumb_ready;
                        item.thumb_uploaded = old.thumb_uploaded;
                        item.duration_label = old.duration_label;
                        item.duration_seconds = old.duration_seconds;
                        break;
                    }
                }
                items_ = std::move(found);
                total_bytes_ = total;
            }
            scanning_ = false;
        }

        std::filesystem::path pending;
        {
            std::scoped_lock lock(mutex_);
            for (const auto& item : items_) {
                if (!item.thumb_ready) {
                    pending = item.path;
                    break;
                }
            }
        }

        if (pending.empty()) {
            ::Sleep(120);
            continue;
        }

        std::vector<std::uint8_t> pixels;
        std::uint32_t w = 0, h = 0;
        double duration = 0.0;
        const Status s = ExtractThumbnail(pending, 256, pixels, w, h, duration);

        std::scoped_lock lock(mutex_);
        for (auto& item : items_) {
            if (item.path != pending) continue;
            item.thumb_ready = true;
            if (s.ok()) {
                item.thumbnail = std::move(pixels);
                item.thumb_width = w;
                item.thumb_height = h;
                item.duration_seconds = duration;
                item.duration_label = FormatDuration(duration);
            } else {
                RF_DEBUG("thumbnail for {} failed: {}", item.display_name, s.str());
            }
            break;
        }
    }

    MFShutdown();
    ::CoUninitialize();
}

}
