#pragma once
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <wrl/client.h>

#include "rf/core/Media.h"
#include "rf/core/Status.h"

namespace rf {

enum class AudioSource {
    SystemLoopback,
    Microphone,
};

struct AudioChunk {
    const float* samples = nullptr;
    std::uint32_t frames = 0;
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;
    Ticks100ns timestamp = 0;
};

using AudioCallback = std::function<void(const AudioChunk&)>;

class WasapiCapture {
public:
    ~WasapiCapture();

    Status Start(AudioSource source, const AudioCallback& on_audio,
                 const std::string& device_id = {});
    void Stop();

    [[nodiscard]] AudioFormat format() const { return format_; }
    [[nodiscard]] std::uint64_t frames_captured() const { return frames_captured_; }
    [[nodiscard]] std::uint64_t silence_filled() const { return silence_filled_; }

private:
    void CaptureLoop();

    Microsoft::WRL::ComPtr<IMMDevice> device_;
    Microsoft::WRL::ComPtr<IAudioClient3> client_;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_;
    HANDLE event_ = nullptr;

    AudioSource source_ = AudioSource::SystemLoopback;
    AudioCallback on_audio_;
    AudioFormat format_{};
    std::vector<float> silence_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::uint64_t frames_captured_ = 0;
    std::uint64_t silence_filled_ = 0;
};

}
