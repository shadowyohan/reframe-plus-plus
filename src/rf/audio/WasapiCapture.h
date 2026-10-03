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
    Application,
};

struct AudioChunk {
    const float* samples = nullptr;
    std::uint32_t frames = 0;
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;
    Ticks100ns timestamp = 0;
};

using AudioCallback = std::function<void(const AudioChunk&)>;

[[nodiscard]] std::uint32_t SilenceFramesOwed(Ticks100ns expected, Ticks100ns now,
                                              std::uint32_t sample_rate);

class WasapiCapture {
public:
    ~WasapiCapture();

    Status Start(AudioSource source, const AudioCallback& on_audio,
                 const std::string& device_id = {});
    Status StartApplication(std::uint32_t process_id, const AudioCallback& on_audio);
    void Stop();

    void ReopenDevice();
    void FollowDefaultDevice();

    [[nodiscard]] AudioFormat format() const { return format_; }
    [[nodiscard]] std::uint64_t frames_captured() const { return frames_captured_; }
    [[nodiscard]] std::uint64_t silence_filled() const { return silence_filled_; }

private:
    Status OpenDevice();
    Status InitializeClient();
    void CloseDevice();
    void StartThread();
    void CaptureLoop();
    void DrainPackets();
    void FillSilenceUntil(Ticks100ns now);
    void TryToRecover(Ticks100ns now);
    void Deliver(const float* samples, std::uint32_t frames, Ticks100ns timestamp);

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<IMMNotificationClient> notifier_;
    Microsoft::WRL::ComPtr<IMMDevice> device_;
    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_;
    HANDLE event_ = nullptr;

    AudioSource source_ = AudioSource::SystemLoopback;
    std::string preferred_device_;
    AudioCallback on_audio_;
    AudioFormat format_{};
    std::vector<float> silence_;
    Ticks100ns expected_ts_ = 0;
    Ticks100ns next_retry_ = 0;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> lost_{false};
    std::atomic<bool> reopen_{false};
    std::atomic<bool> using_preferred_{false};
    std::uint64_t frames_captured_ = 0;
    std::uint64_t silence_filled_ = 0;
};

}
