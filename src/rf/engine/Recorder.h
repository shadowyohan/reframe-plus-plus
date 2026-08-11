#pragma once
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>

#include "rf/audio/AacEncoder.h"
#include "rf/audio/WasapiCapture.h"
#include "rf/capture/GameWatcher.h"
#include "rf/capture/IVideoCapture.h"
#include "rf/encode/IVideoEncoder.h"
#include "rf/engine/Settings.h"
#include "rf/mux/Mp4Muxer.h"
#include "rf/replay/ReplayBuffer.h"

namespace rf {

class Recorder {
public:
    enum class State { Idle, ReplayArmed, Recording };

    struct Status_ {
        State state = State::Idle;
        CaptureStats capture{};
        EncoderStats encode{};
        ReplayBuffer::Stats replay{};
        std::string capture_backend;
        std::string encoder_backend;
        std::string gpu;
        std::string quirks;
    };

    Recorder();
    ~Recorder();

    Status Init(const Settings& settings);
    void Shutdown();

    Status ApplySettings(const Settings& settings);

    Status ArmReplay();
    void DisarmReplay();

    Status SaveReplay(std::filesystem::path* saved_to = nullptr, std::uint32_t seconds = 0);

    Status StartRecording(const std::filesystem::path& file);
    Status StopRecording();

    [[nodiscard]] State state() const { return state_.load(); }

    [[nodiscard]] const D3DDevicePtr& device() const { return device_; }
    [[nodiscard]] Status_ GetStatus() const;

    [[nodiscard]] std::string target_app() const;
    [[nodiscard]] const Settings& settings() const { return settings_; }

private:

    Status ArmReplayLocked();
    void DisarmReplayLocked();

    void OnGameChanged(const GameWindow& game);

    Status BuildPipeline();
    void OnFrame(const CapturedFrame& frame);
    void OnPacket(PacketPtr packet);
    std::filesystem::path MakeOutputPath(const char* suffix) const;

    void StartPacer();
    void StopPacer();
    void PacerLoop();

    void SubmitPacedLocked(const CapturedFrame& frame, Ticks100ns period);

    void OnSystemAudio(const AudioChunk& chunk);
    void OnMicAudio(const AudioChunk& chunk);

    Settings settings_;
    D3DDevicePtr device_;

    std::mutex pipeline_mutex_;
    GameWatcher game_watcher_;
    QuirkSet quirks_;
    VideoCapturePtr capture_;
    VideoEncoderPtr encoder_;
    std::unique_ptr<WasapiCapture> system_audio_;
    std::unique_ptr<WasapiCapture> microphone_;
    ReplayBuffer replay_;

    std::mutex mux_mutex_;
    std::unique_ptr<Mp4Muxer> muxer_;

    std::atomic<State> state_{State::Idle};
    std::atomic<bool> encoder_ready_{false};

    std::mutex pace_mutex_;
    Ticks100ns last_pts_ = 0;
    Ticks100ns next_deadline_ = 0;
    bool have_last_ = false;
    Ticks100ns pacer_period_ = 0;

    bool mf_started_ = false;
    std::thread pacer_;
    std::atomic<bool> pacing_{false};
    std::uint64_t paced_index_ = 0;
    HANDLE frame_event_ = nullptr;
    Ticks100ns epoch_ = 0;

    std::unique_ptr<AacEncoder> aac_;
    std::mutex mic_mutex_;
    std::vector<float> mic_fifo_;
    std::vector<float> mix_scratch_;
    std::atomic<float> system_volume_{1.0f};
    std::atomic<float> mic_volume_{1.0f};
    VideoFormat video_format_{};
};

}
