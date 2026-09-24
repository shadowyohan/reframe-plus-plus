#include "rf/audio/NoiseSuppressor.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <mutex>

#include <rnnoise.h>
#include <speex/speex_preprocess.h>

#include "rf/core/Log.h"
#include "rf/core/Strings.h"

namespace rf {
namespace {

constexpr float kInt16Scale = 32767.0f;
constexpr int kSpeexSuppressDb = -30;
constexpr std::uint32_t kSampleRate = 48'000;
constexpr float kMaxineIntensity = 1.0f;

RNNModel* EmbeddedRnnoiseModel() {
    static RNNModel* model = [] () -> RNNModel* {
        HRSRC resource = ::FindResourceW(nullptr, L"RNNOISE_MODEL", RT_RCDATA);
        HGLOBAL loaded = resource ? ::LoadResource(nullptr, resource) : nullptr;
        const void* blob = loaded ? ::LockResource(loaded) : nullptr;
        if (!blob) return nullptr;
        return rnnoise_model_from_buffer(blob, static_cast<int>(::SizeofResource(nullptr, resource)));
    }();
    return model;
}

class RnnoiseSuppressor final : public NoiseSuppressor {
public:
    explicit RnnoiseSuppressor(DenoiseState* state) : state_(state) {}
    ~RnnoiseSuppressor() override { rnnoise_destroy(state_); }

    void Process(float* mono_frame) override {
        for (std::uint32_t i = 0; i < kDenoiseFrame; ++i) scaled_[i] = mono_frame[i] * kInt16Scale;
        rnnoise_process_frame(state_, scaled_.data(), scaled_.data());
        for (std::uint32_t i = 0; i < kDenoiseFrame; ++i) mono_frame[i] = scaled_[i] / kInt16Scale;
    }

private:
    DenoiseState* state_;
    std::array<float, kDenoiseFrame> scaled_{};
};

class SpeexSuppressor final : public NoiseSuppressor {
public:
    explicit SpeexSuppressor(SpeexPreprocessState* state) : state_(state) {}
    ~SpeexSuppressor() override { speex_preprocess_state_destroy(state_); }

    void Process(float* mono_frame) override {
        for (std::uint32_t i = 0; i < kDenoiseFrame; ++i)
            pcm_[i] = static_cast<spx_int16_t>(std::clamp(mono_frame[i], -1.0f, 1.0f) * kInt16Scale);
        speex_preprocess_run(state_, pcm_.data());
        for (std::uint32_t i = 0; i < kDenoiseFrame; ++i) mono_frame[i] = pcm_[i] / kInt16Scale;
    }

private:
    SpeexPreprocessState* state_;
    std::array<spx_int16_t, kDenoiseFrame> pcm_{};
};

struct MaxineApi {
    using Handle = void*;
    using CreateEffect = int(__cdecl*)(const char*, Handle*);
    using DestroyEffect = int(__cdecl*)(Handle);
    using SetU32 = int(__cdecl*)(Handle, const char*, unsigned);
    using SetString = int(__cdecl*)(Handle, const char*, const char*);
    using SetFloat = int(__cdecl*)(Handle, const char*, float);
    using Load = int(__cdecl*)(Handle);
    using Run = int(__cdecl*)(Handle, const float**, float**, unsigned, unsigned);

    CreateEffect create = nullptr;
    DestroyEffect destroy = nullptr;
    SetU32 set_u32 = nullptr;
    SetString set_string = nullptr;
    SetFloat set_float = nullptr;
    Load load = nullptr;
    Run run = nullptr;

    [[nodiscard]] bool complete() const {
        return create && destroy && set_u32 && set_string && set_float && load && run;
    }
};

const MaxineApi* LoadMaxineApi() {
    static MaxineApi api;
    static std::once_flag once;
    std::call_once(once, [] {
        const auto dll = MaxineSdkDir() / L"NVAudioEffects.dll";
        HMODULE module = ::LoadLibraryExW(dll.c_str(), nullptr,
                                          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                              LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) {
            RF_WARN("NVIDIA Maxine: cannot load {} ({})", dll.string(), ::GetLastError());
            return;
        }
        auto resolve = [module]<typename Fn>(Fn& fn, const char* name) {
            fn = reinterpret_cast<Fn>(::GetProcAddress(module, name));
        };
        resolve(api.create, "NvAFX_CreateEffect");
        resolve(api.destroy, "NvAFX_DestroyEffect");
        resolve(api.set_u32, "NvAFX_SetU32");
        resolve(api.set_string, "NvAFX_SetString");
        resolve(api.set_float, "NvAFX_SetFloat");
        resolve(api.load, "NvAFX_Load");
        resolve(api.run, "NvAFX_Run");
    });
    return api.complete() ? &api : nullptr;
}

std::filesystem::path MaxineDenoiserModel() {
    return MaxineSdkDir() / L"models" / L"denoiser_48k.trtpkg";
}

class MaxineSuppressor final : public NoiseSuppressor {
public:
    MaxineSuppressor(const MaxineApi& api, MaxineApi::Handle handle) : api_(api), handle_(handle) {}
    ~MaxineSuppressor() override { api_.destroy(handle_); }

    void Process(float* mono_frame) override {
        std::copy_n(mono_frame, kDenoiseFrame, input_.begin());
        const float* input = input_.data();
        float* output = mono_frame;
        if (const int status = api_.run(handle_, &input, &output, kDenoiseFrame, 1);
            status != 0 && !reported_) {
            reported_ = true;
            RF_WARN("NVIDIA Maxine denoiser failed with status {}", status);
        }
    }

private:
    const MaxineApi& api_;
    MaxineApi::Handle handle_;
    std::array<float, kDenoiseFrame> input_{};
    bool reported_ = false;
};

Status CreateMaxine(std::unique_ptr<NoiseSuppressor>& out) {
    const MaxineApi* api = LoadMaxineApi();
    if (!api) return Status::Fail("NVIDIA Maxine is not installed");

    MaxineApi::Handle handle = nullptr;
    if (api->create("denoiser", &handle) != 0 || !handle)
        return Status::Fail("NVIDIA Maxine: cannot create the denoiser");

    const std::string model = ToUtf8(MaxineDenoiserModel().wstring());
    const bool configured = api->set_u32(handle, "input_sample_rate", kSampleRate) == 0 &&
                            api->set_float(handle, "intensity_ratio", kMaxineIntensity) == 0 &&
                            api->set_string(handle, "model_path", model.c_str()) == 0 &&
                            api->load(handle) == 0;
    if (!configured) {
        api->destroy(handle);
        return Status::Fail("NVIDIA Maxine: the denoiser model did not load");
    }
    out = std::make_unique<MaxineSuppressor>(*api, handle);
    return Status::Ok();
}

}

std::filesystem::path MaxineSdkDir() {
    for (const wchar_t* variable : {L"NVAFX_SDK_DIR", L"AFX_SDK_DIR"}) {
        wchar_t value[MAX_PATH]{};
        if (::GetEnvironmentVariableW(variable, value, MAX_PATH) > 0) return value;
    }
    wchar_t program_files[MAX_PATH]{};
    ::GetEnvironmentVariableW(L"ProgramFiles", program_files, MAX_PATH);
    return std::filesystem::path(program_files) / L"NVIDIA Corporation" /
           L"NVIDIA Audio Effects SDK";
}

bool MaxineInstalled() {
    std::error_code ec;
    return std::filesystem::exists(MaxineSdkDir() / L"NVAudioEffects.dll", ec) &&
           std::filesystem::exists(MaxineDenoiserModel(), ec);
}

Status CreateNoiseSuppressor(NoiseSuppression kind, std::unique_ptr<NoiseSuppressor>& out) {
    switch (kind) {
        case NoiseSuppression::RNNoise: {
            RNNModel* model = EmbeddedRnnoiseModel();
            if (!model) return Status::Fail("the RNNoise model is missing from the executable");
            DenoiseState* state = rnnoise_create(model);
            if (!state) return Status::Fail("RNNoise rejected its model");
            out = std::make_unique<RnnoiseSuppressor>(state);
            return Status::Ok();
        }
        case NoiseSuppression::Speex: {
            SpeexPreprocessState* state =
                speex_preprocess_state_init(static_cast<int>(kDenoiseFrame), kSampleRate);
            if (!state) return Status::Fail("Speex preprocessor unavailable");
            spx_int32_t enabled = 1;
            spx_int32_t suppress = kSpeexSuppressDb;
            speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_DENOISE, &enabled);
            speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &suppress);
            out = std::make_unique<SpeexSuppressor>(state);
            return Status::Ok();
        }
        case NoiseSuppression::Maxine:
            return CreateMaxine(out);
    }
    return Status::Fail("unknown noise suppression");
}

Status MicDenoiser::Open(NoiseSuppression kind) {
    engine_.reset();
    RF_TRY(CreateNoiseSuppressor(kind, engine_));
    pending_.clear();
    ready_.assign(kDenoiseFrame, 0.0f);
    return Status::Ok();
}

void MicDenoiser::Process(float* interleaved, std::uint32_t frames, std::uint32_t channels) {
    if (!engine_ || !interleaved || channels == 0) return;

    for (std::uint32_t i = 0; i < frames; ++i) {
        float sum = 0.0f;
        for (std::uint32_t c = 0; c < channels; ++c) sum += interleaved[i * channels + c];
        pending_.push_back(sum / static_cast<float>(channels));
    }

    std::size_t consumed = 0;
    while (pending_.size() - consumed >= kDenoiseFrame) {
        float* frame = pending_.data() + consumed;
        engine_->Process(frame);
        ready_.insert(ready_.end(), frame, frame + kDenoiseFrame);
        consumed += kDenoiseFrame;
    }
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(consumed));

    for (std::uint32_t i = 0; i < frames; ++i)
        for (std::uint32_t c = 0; c < channels; ++c) interleaved[i * channels + c] = ready_[i];
    ready_.erase(ready_.begin(), ready_.begin() + frames);
}

}
