#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>

#include "rf/audio/AacEncoder.h"
#include "rf/audio/AppAudioTracks.h"
#include "rf/audio/NoiseSuppressor.h"
#include "rf/audio/ProcessAudioTrack.h"
#include "rf/audio/WasapiCapture.h"
#include "rf/capture/GameCapture.h"
#include "rf/capture/GameWatcher.h"
#include "rf/capture/IVideoCapture.h"
#include "rf/encode/IVideoEncoder.h"
#include "rf/engine/Settings.h"
#include "rf/gpu/ColorConverter.h"
#include "rf/gpu/FrameBridge.h"
#include "rf/mux/ClipCropper.h"
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
    Status RebuildPipeline();

    Status ArmReplay();
    void DisarmReplay();

    Status SaveReplay(std::filesystem::path* saved_to = nullptr, std::uint32_t seconds = 0,
                      bool release_saved = true);

    struct GameClip {
        std::filesystem::path path;
        std::uint32_t seconds = 0;
        bool needs_crop = false;
        CropJob crop;
    };
    Status SaveGameClip(const std::string& app, const std::string& tag, std::uint32_t seconds,
                        GameClip& out);

    void SetGameAudio(std::uint32_t pid);
    void SampleGameWindow(std::uint32_t pid);

    Status StartRecording(const std::filesystem::path& file);
    Status StopRecording();

    [[nodiscard]] bool encoder_failed() const { return encoder_failed_.load(); }
    Status RecoverEncoder();

    [[nodiscard]] State state() const { return state_.load(); }

    [[nodiscard]] const D3DDevicePtr& device() const { return device_; }
    [[nodiscard]] const D3DDevicePtr& encode_device() const { return encode_device_; }
    [[nodiscard]] Status_ GetStatus() const;

    [[nodiscard]] std::string target_app() const;

    [[nodiscard]] bool capture_is_hooked() const;

    Status SetCaptureMonitor(void* hmonitor);
    [[nodiscard]] void* capture_monitor() const { return active_monitor_; }

    void SetInGameOverlay(std::uint32_t handle, std::uint32_t serial, std::uint32_t width,
                          std::uint32_t height, bool visible);
    [[nodiscard]] const Settings& settings() const { return settings_; }

    std::vector<std::string> TakeOverflowedApps() { return app_tracks_.TakeOverflowedApps(); }

    [[nodiscard]] static bool PacerAccepts(Ticks100ns timestamp, Ticks100ns deadline,
                                           Ticks100ns period) {
        return timestamp + period / 2 >= deadline;
    }

    [[nodiscard]] static Ticks100ns SmoothedSlot(Ticks100ns ideal, Ticks100ns arrived,
                                                 Ticks100ns period) {
        const Ticks100ns error = arrived - ideal;
        if (error > period * 4 || error < -period * 4) return arrived;
        return ideal + error / 16;
    }

private:

    Status ArmReplayLocked();
    Status StopRecordingLocked();
    Status SetCaptureMonitorLocked(void* hmonitor);
    void NoteSubmit(const Status& submitted);

    Status StartDisplayCapture(const CaptureTarget& target, const FrameCallback& on_frame,
                               VideoCapturePtr& out);
    void DisarmReplayLocked();

    void OnGameChanged(const GameWindow& game);

    Status BuildPipeline();
    void OnFrame(const CapturedFrame& frame);
    void OnPacket(PacketPtr packet);
    void StartWriter();
    void StopWriter();
    void FlushWriter();
    void WriterLoop();
    std::filesystem::path MakeOutputPath(std::string_view label, std::string_view tail = {}) const;
    Status WriteClipLocked(const std::filesystem::path& path, Ticks100ns window,
                           IMFMediaType* audio_type, std::uint32_t audio_tracks,
                           const std::vector<std::uint32_t>& kept_tracks, Ticks100ns* covered_to);

    struct CaptureGeometry {
        bool valid = false;
        bool display = false;
        bool hooked = false;
        HWND window = nullptr;
        RECT monitor{};
        std::uint32_t source_width = 0, source_height = 0;
        std::uint32_t video_width = 0, video_height = 0;
    };
    void RememberGeometry(const CaptureTarget& target, bool hooked);
    [[nodiscard]] HWND GameWindowOf(std::uint32_t pid);
    [[nodiscard]] std::vector<WindowSample> WindowSamplesBetween(Ticks100ns from,
                                                                 Ticks100ns to) const;

    void StartSystemAudioLocked();
    void StartGameAudioLocked();
    void StopGameAudioLocked();
    void FeedGameAudio(std::uint32_t frames, Ticks100ns timestamp);

    void StartPacer();
    void StopPacer();
    void PacerLoop();

    void SubmitPacedLocked(const CapturedFrame& frame, Ticks100ns period);
    [[nodiscard]] Ticks100ns SubmitPeriod() const;

    void OnSystemAudio(const AudioChunk& chunk);
    void OnMicAudio(const AudioChunk& chunk);
    [[nodiscard]] std::uint32_t AudioTrackCount() const;
    [[nodiscard]] bool MicInMainTrack() const;
    [[nodiscard]] std::uint32_t FirstAppTrack() const;
    [[nodiscard]] std::vector<std::uint32_t> AudibleAudioTracks(Ticks100ns since) const;
    [[nodiscard]] std::vector<std::string> AudioTrackNames(
        const std::vector<std::uint32_t>& tracks) const;
    void FinishRecordingInBackground(std::filesystem::path file,
                                     std::vector<std::uint32_t> kept_tracks,
                                     std::vector<std::string> names, bool drop_silent);

    Settings settings_;
    D3DDevicePtr device_;
    D3DDevicePtr encode_device_;

    std::mutex pipeline_mutex_;
    GameWatcher game_watcher_;

    void* active_monitor_ = nullptr;
    ColorConverter scaler_;
    FrameBridge bridge_;
    bool bridge_ready_ = false;
    std::uint64_t bridge_misses_ = 0;
    std::uint32_t scaler_src_width_ = 0;
    std::uint32_t scaler_src_height_ = 0;
    QuirkSet quirks_;
    VideoCapturePtr capture_;

    std::unique_ptr<GameCapture> overlay_hook_;
    VideoEncoderPtr encoder_;
    std::unique_ptr<WasapiCapture> system_audio_;
    std::unique_ptr<WasapiCapture> microphone_;
    ReplayBuffer replay_;

    std::mutex mux_mutex_;
    std::unique_ptr<Mp4Muxer> muxer_;
    Ticks100ns recording_from_ = 0;
    std::thread trimmer_;

    std::thread writer_;
    std::atomic<bool> writing_{false};
    std::mutex writer_mutex_;
    std::condition_variable writer_cv_;
    std::condition_variable writer_drained_;
    std::deque<PacketPtr> writer_queue_;
    std::uint64_t writer_dropped_ = 0;

    std::atomic<State> state_{State::Idle};

    bool arm_requested_ = false;
    std::atomic<bool> encoder_ready_{false};
    bool force_software_encoder_ = false;
    Ticks100ns last_encoder_recovery_ = 0;
    std::atomic<std::uint64_t> submit_failures_{0};
    std::atomic<bool> encoder_failed_{false};

    std::mutex pace_mutex_;
    Ticks100ns last_pts_ = 0;
    Ticks100ns next_deadline_ = 0;
    bool have_last_ = false;
    Ticks100ns pacer_period_ = 0;
    std::atomic<std::uint32_t> submit_divider_{1};

    bool mf_started_ = false;
    std::thread pacer_;
    std::atomic<bool> pacing_{false};
    std::uint64_t paced_index_ = 0;
    std::atomic<std::uint64_t> frames_offered_{0};
    std::atomic<std::uint64_t> frames_paced_{0};
    std::atomic<std::uint64_t> frames_filled_{0};
    HANDLE frame_event_ = nullptr;
    Ticks100ns epoch_ = 0;

    std::unique_ptr<AacEncoder> aac_;

    std::unique_ptr<AacEncoder> aac_mic_;
    std::unique_ptr<AacEncoder> aac_system_;
    AppAudioTracks app_tracks_;
    std::mutex game_audio_mutex_;
    std::unique_ptr<ProcessAudioTrack> game_audio_;
    std::uint32_t game_audio_pid_ = 0;

    mutable std::mutex window_mutex_;
    CaptureGeometry geometry_;
    std::deque<WindowSample> window_samples_;
    HWND game_window_ = nullptr;
    std::uint32_t game_window_pid_ = 0;
    Ticks100ns game_window_checked_ = 0;
    MicDenoiser denoiser_;
    std::mutex mic_mutex_;
    std::vector<float> mic_fifo_;
    std::vector<float> mix_scratch_;
    std::vector<float> mic_scratch_;
    std::vector<float> mic_gained_;
    std::atomic<float> system_volume_{1.0f};
    std::atomic<float> mic_volume_{1.0f};
    VideoFormat video_format_{};
};

}
