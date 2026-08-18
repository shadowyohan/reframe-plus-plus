#include "rf/engine/Recorder.h"

#include <dwmapi.h>
#include <mfapi.h>

#include <chrono>
#include <format>

#include "rf/capture/GameCapture.h"
#include "rf/core/Log.h"
#include "rf/core/Strings.h"

namespace rf {

Recorder::Recorder() = default;
Recorder::~Recorder() { Shutdown(); }

Status Recorder::Init(const Settings& settings) {
    settings_ = settings;

    RF_HR(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    mf_started_ = true;

    if (settings_.has_gpu_override()) {
        RF_TRY(D3DDevice::CreateForLuid(settings_.gpu_luid_low, settings_.gpu_luid_high, device_));
    } else {
        RF_TRY(D3DDevice::CreateForOutput(nullptr, device_));
    }
    device_->SetLowGpuPriority();

    quirks_ = DetectQuirks(device_->info());

    replay_.Configure(static_cast<Ticks100ns>(settings_.replay_seconds) * kOneSecond100ns,
                      static_cast<std::size_t>(settings_.replay_max_memory_mb) * 1024 * 1024,
                      settings_.temp_dir);

    RF_INFO("recorder initialised on {} ({})", ToUtf8(device_->info().description),
            ToString(device_->info().vendor));
    return Status::Ok();
}

Status Recorder::ApplySettings(const Settings& next) {
    std::scoped_lock lock(pipeline_mutex_);

    const bool pipeline_differs =
        next.ResolvedFps() != settings_.ResolvedFps() ||
        next.ResolvedHeight() != settings_.ResolvedHeight() ||
        next.bitrate_kbps != settings_.bitrate_kbps || next.codec != settings_.codec ||
        next.keyframe_interval_ms != settings_.keyframe_interval_ms ||
        next.capture_focused_window_only != settings_.capture_focused_window_only ||
        next.capture_backend != settings_.capture_backend ||
        next.capture_cursor != settings_.capture_cursor ||
        next.encoder_backend != settings_.encoder_backend ||
        next.record_system_audio != settings_.record_system_audio ||
        next.record_microphone != settings_.record_microphone ||
        next.mic_device != settings_.mic_device ||
        next.audio_bitrate_kbps != settings_.audio_bitrate_kbps ||
        next.audio_tracks != settings_.audio_tracks || next.hdr != settings_.hdr;

    const bool monitor_changed = next.capture_monitor != settings_.capture_monitor;

    settings_ = next;
    replay_.Configure(static_cast<Ticks100ns>(settings_.replay_seconds) * kOneSecond100ns,
                      static_cast<std::size_t>(settings_.replay_max_memory_mb) * 1024 * 1024,
                      settings_.temp_dir);

    system_volume_ = settings_.system_volume;
    mic_volume_ = settings_.mic_volume * settings_.mic_gain;

    if (monitor_changed && !settings_.capture_focused_window_only) {
        if (Status s = SetCaptureMonitorLocked(MonitorForDeviceName(ToWide(settings_.capture_monitor)));
            !s)
            RF_WARN("display switch: {}", s.str());
    }

    if (!pipeline_differs || state_ != State::ReplayArmed) return Status::Ok();

    RF_INFO("settings changed - rebuilding the pipeline");
    DisarmReplayLocked();
    return ArmReplayLocked();
}

Status Recorder::StartDisplayCapture(const CaptureTarget& target, const FrameCallback& on_frame,
                                     VideoCapturePtr& out) {
    VideoCapturePtr fresh;
    RF_TRY(CreateVideoCapture(device_, settings_.capture_backend, target, fresh));

    Status started = fresh->Start(target, on_frame);
    if (!started) {

        const bool can_fall_back = fresh->backend() == CaptureBackend::DesktopDuplication &&
                                   settings_.capture_backend == CaptureBackend::Auto &&
                                   IsWgcSupported();
        if (!can_fall_back) return started;

        RF_WARN("desktop duplication unavailable ({}) - falling back to "
                "Windows.Graphics.Capture",
                started.str());
        fresh.reset();
        RF_TRY(CreateVideoCapture(device_, CaptureBackend::WindowsGraphicsCapture, target, fresh));
        RF_TRY(fresh->Start(target, on_frame));
    }

    out = std::move(fresh);
    return Status::Ok();
}

Status Recorder::BuildPipeline() {
    CaptureTarget target;
    target.kind = settings_.capture_focused_window_only ? CaptureTarget::Kind::Window
                                                        : CaptureTarget::Kind::Display;
    if (target.kind == CaptureTarget::Kind::Window) {

        const GameWindow game = game_watcher_.current();
        target.hwnd = game.valid() ? game.hwnd : ::GetForegroundWindow();
    } else {

        if (!active_monitor_)
            active_monitor_ = MonitorForDeviceName(ToWide(settings_.capture_monitor));
        target.hmonitor = active_monitor_;

        for (const MonitorInfo& info : EnumerateMonitors()) {
            if (info.handle != active_monitor_) continue;
            RF_INFO("recording display {} ({}) at {},{} {}x{}{}", ToUtf8(info.device_name),
                    ToUtf8(info.description), info.x, info.y, info.width, info.height,
                    settings_.capture_monitor.empty() ? " - none configured, using the primary"
                                                      : "");
        }
    }
    target.capture_cursor = settings_.capture_cursor;
    target.fps = settings_.ResolvedFps();

    auto on_frame = [this](const CapturedFrame& f) { OnFrame(f); };

    bool hooked = false;
    if (target.kind == CaptureTarget::Kind::Window &&
        settings_.capture_backend == CaptureBackend::Auto) {
        VideoCapturePtr hook;
        if (Status s = CreateVideoCapture(device_, CaptureBackend::GameHook, target, hook); s) {
            if (Status started = hook->Start(target, on_frame); started) {
                capture_ = std::move(hook);
                hooked = true;
            } else {
                RF_WARN("game capture unavailable ({}) - falling back to window capture",
                        started.message());
            }
        }
    }

    if (!hooked) RF_TRY(StartDisplayCapture(target, on_frame, capture_));

    if (!hooked && target.kind == CaptureTarget::Kind::Window && target.hwnd) {
        auto hook = std::make_unique<GameCapture>(device_);
        if (Status s = hook->Attach(target); s) {
            overlay_hook_ = std::move(hook);
            RF_INFO("hook attached for the in-game overlay only");
        } else {
            RF_WARN("no in-game overlay: {}", s.message());
        }
    }

    video_format_.width = capture_->width();
    video_format_.height = capture_->height();

    if (const std::uint32_t target_height = settings_.ResolvedHeight();
        target_height != 0 && target_height < video_format_.height) {
        const double aspect = static_cast<double>(capture_->width()) / capture_->height();
        video_format_.height = target_height;
        video_format_.width = static_cast<std::uint32_t>(target_height * aspect + 0.5);
    }

    video_format_.width &= ~1u;
    video_format_.height &= ~1u;
    video_format_.fps_num = settings_.ResolvedFps();
    video_format_.fps_den = 1;
    video_format_.codec = settings_.codec;
    video_format_.color = settings_.hdr ? ColorSpace::Rec2020Pq : ColorSpace::Rec709;

    EncoderConfig cfg;
    cfg.format = video_format_;
    cfg.rate_control = settings_.rate_control;
    cfg.bitrate_kbps = settings_.bitrate_kbps;
    cfg.keyframe_interval_ms = settings_.keyframe_interval_ms;

    cfg.quality_vs_speed = 66;
    cfg.quirks = quirks_;
    cfg.input_width = capture_->width();
    cfg.input_height = capture_->height();

    epoch_ = Now100ns();
    cfg.epoch = epoch_;

    pacer_period_ = 0;
    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof(timing);
    if (capture_->backend() != CaptureBackend::GameHook &&
        SUCCEEDED(::DwmGetCompositionTimingInfo(nullptr, &timing)) &&
        timing.rateRefresh.uiDenominator && timing.rateRefresh.uiNumerator) {
        const double display_hz = static_cast<double>(timing.rateRefresh.uiNumerator) /
                                  timing.rateRefresh.uiDenominator;

        if (display_hz > 10.0 &&
            video_format_.fps_num > static_cast<std::uint32_t>(display_hz * 1.05)) {
            RF_WARN("{} fps requested but the display refreshes at {:.0f} Hz - capping "
                    "(frames above refresh would all be duplicates)",
                    video_format_.fps_num, display_hz);
            video_format_.fps_num = static_cast<std::uint32_t>(display_hz + 0.5);
        }

        const double target_hz =
            static_cast<double>(video_format_.fps_num) / std::max(1u, video_format_.fps_den);
        if (std::abs(display_hz - target_hz) / target_hz < 0.02) {
            pacer_period_ = static_cast<Ticks100ns>(kOneSecond100ns) *
                            timing.rateRefresh.uiDenominator / timing.rateRefresh.uiNumerator;
            RF_INFO("pacer locked to the display: {:.3f} Hz (target {})", display_hz,
                    video_format_.fps_num);
        }
    }

    cfg.format = video_format_;

    RF_TRY(CreateVideoEncoder(device_, settings_.encoder_backend, encoder_));
    auto on_packet = [this](PacketPtr p) { OnPacket(std::move(p)); };

    if (auto s = encoder_->Open(cfg, on_packet); !s.ok()) {

        RF_WARN("{} unavailable ({}) - falling back to Media Foundation",
                ToString(encoder_->backend()), s.str());
        encoder_.reset();
        RF_TRY(CreateVideoEncoder(device_, EncoderBackend::MediaFoundation, encoder_));
        RF_TRY(encoder_->Open(cfg, on_packet));
    }
    encoder_ready_.store(true, std::memory_order_release);

    system_volume_ = settings_.system_volume;
    mic_volume_ = settings_.mic_volume * settings_.mic_gain;

    if (settings_.record_system_audio) {
        aac_ = std::make_unique<AacEncoder>();
        AudioFormat afmt;
        if (auto s = aac_->Open(afmt, settings_.audio_bitrate_kbps * 1000, epoch_, on_packet);
            !s.ok()) {
            RF_WARN("AAC encoder unavailable ({}) - recording without sound", s.str());
            aac_.reset();
    aac_mic_.reset();
        }

        if (settings_.separate_audio_tracks() && settings_.record_microphone) {
            aac_mic_ = std::make_unique<AacEncoder>();
            AudioFormat mic_fmt;
            if (auto s = aac_mic_->Open(mic_fmt, settings_.audio_bitrate_kbps * 1000, epoch_,
                                        on_packet, 1);
                !s.ok()) {
                RF_WARN("second audio track unavailable ({}) - mixing instead", s.str());
                aac_mic_.reset();
            }
        }

        system_audio_ = std::make_unique<WasapiCapture>();
        if (auto s = system_audio_->Start(AudioSource::SystemLoopback,
                                          [this](const AudioChunk& c) { OnSystemAudio(c); });
            !s.ok()) {
            RF_WARN("system audio unavailable: {}", s.str());
            system_audio_.reset();
        }
    }

    if (settings_.record_microphone) {
        microphone_ = std::make_unique<WasapiCapture>();
        if (auto s = microphone_->Start(AudioSource::Microphone,
                                        [this](const AudioChunk& c) { OnMicAudio(c); },
                                        settings_.mic_device);
            !s.ok()) {
            RF_WARN("microphone unavailable: {}", s.str());
            microphone_.reset();
        }
    }

    RF_INFO("pipeline up: {} -> {} at {}x{}@{}", ToString(capture_->backend()), encoder_->name(),
            video_format_.width, video_format_.height, video_format_.fps_num);
    return Status::Ok();
}

Status Recorder::ArmReplay() {
    std::scoped_lock lock(pipeline_mutex_);
    arm_requested_ = true;
    return ArmReplayLocked();
}

Status Recorder::ArmReplayLocked() {
    if (state_ != State::Idle) return Status::Fail("already running");

    if (settings_.capture_focused_window_only && !game_watcher_.running())
        game_watcher_.Start([this](const GameWindow& g) { OnGameChanged(g); });

    if (settings_.capture_focused_window_only && !game_watcher_.current().valid()) {

        RF_INFO("replay armed - waiting for an application to record");
        state_ = State::ReplayArmed;
        return Status::Ok();
    }

    RF_TRY(BuildPipeline());
    StartPacer();
    state_ = State::ReplayArmed;
    return Status::Ok();
}

void Recorder::DisarmReplay() {
    std::scoped_lock lock(pipeline_mutex_);
    arm_requested_ = false;
    DisarmReplayLocked();
}

void Recorder::OnGameChanged(const GameWindow& game) {
    std::scoped_lock lock(pipeline_mutex_);

    const bool retargetable = state_ == State::ReplayArmed || (arm_requested_ && state_ == State::Idle);
    if (!retargetable || !settings_.capture_focused_window_only) return;

    if (game.valid())
        RF_INFO("retargeting capture to \"{}\" ({}x{})", game.exe, game.width, game.height);
    else
        RF_INFO("game gone - capture idle until the next one");

    DisarmReplayLocked();
    if (Status s = ArmReplayLocked(); !s)
        RF_WARN("could not retarget capture: {}", s.str());
}

void Recorder::DisarmReplayLocked() {
    StopPacer();
    encoder_ready_.store(false, std::memory_order_release);
    if (capture_) capture_->Stop();
    if (encoder_) encoder_->Close();
    if (system_audio_) system_audio_->Stop();
    if (microphone_) microphone_->Stop();
    if (aac_) aac_->Close();
    if (aac_mic_) aac_mic_->Close();
    capture_.reset();
    overlay_hook_.reset();
    encoder_.reset();
    system_audio_.reset();
    microphone_.reset();
    aac_.reset();
    {
        std::scoped_lock lock(mic_mutex_);
        mic_fifo_.clear();
    }
    replay_.Clear();
    state_ = State::Idle;
}

void Recorder::OnSystemAudio(const AudioChunk& chunk) {
    if (!aac_ || !chunk.samples || chunk.frames == 0) return;

    const std::size_t samples = static_cast<std::size_t>(chunk.frames) * chunk.channels;
    mix_scratch_.assign(chunk.samples, chunk.samples + samples);

    const float sys_vol = system_volume_.load(std::memory_order_relaxed);
    for (float& v : mix_scratch_) v *= sys_vol;

    {
        std::scoped_lock lock(mic_mutex_);
        const float mic_vol = mic_volume_.load(std::memory_order_relaxed);
        const std::size_t take = std::min(mic_fifo_.size(), samples);
        for (std::size_t i = 0; i < take; ++i) mix_scratch_[i] += mic_fifo_[i] * mic_vol;
        mic_fifo_.erase(mic_fifo_.begin(), mic_fifo_.begin() + take);
    }

    if (auto s = aac_->Feed(mix_scratch_.data(), chunk.frames, chunk.timestamp); !s.ok())
        RF_WARN("audio encode: {}", s.str());
}

void Recorder::OnMicAudio(const AudioChunk& chunk) {
    if (!chunk.samples || chunk.frames == 0) return;

    if (aac_mic_) {
        const float gain = mic_volume_.load(std::memory_order_relaxed);
        const std::size_t count = static_cast<std::size_t>(chunk.frames) * chunk.channels;
        mic_scratch_.assign(chunk.samples, chunk.samples + count);
        for (float& v : mic_scratch_) v *= gain;
        if (auto s = aac_mic_->Feed(mic_scratch_.data(), chunk.frames, chunk.timestamp); !s.ok())
            RF_WARN("microphone encode: {}", s.str());
        return;
    }

    std::scoped_lock lock(mic_mutex_);
    const std::size_t samples = static_cast<std::size_t>(chunk.frames) * chunk.channels;
    mic_fifo_.insert(mic_fifo_.end(), chunk.samples, chunk.samples + samples);

    constexpr std::size_t kCap = 48'000 / 4 * 2;
    if (mic_fifo_.size() > kCap)
        mic_fifo_.erase(mic_fifo_.begin(), mic_fifo_.end() - kCap);
}

void Recorder::SubmitPacedLocked(const CapturedFrame& frame, Ticks100ns period) {
    auto submit = [&](Ticks100ns pts, bool changed) {
        CapturedFrame out = frame;
        out.timestamp = pts;
        out.frame_index = paced_index_++;

        out.content_changed = changed;
        if (auto s = encoder_->Submit(out); !s.ok()) RF_WARN("encoder submit: {}", s.str());
    };

    if (!have_last_) {
        submit(frame.timestamp, true);
        last_pts_ = frame.timestamp;
        next_deadline_ = frame.timestamp + period;
        have_last_ = true;
        return;
    }

    if (!PacerAccepts(frame.timestamp, next_deadline_, period)) return;

    submit(frame.timestamp, true);
    last_pts_ = frame.timestamp;
    next_deadline_ += period;
    if (next_deadline_ <= frame.timestamp) next_deadline_ = frame.timestamp + period;
}

void Recorder::OnFrame(const CapturedFrame& frame) {
    if (!encoder_ready_.load(std::memory_order_acquire)) return;

    CapturedFrame fitted = frame;
    if (frame.texture && (frame.width != video_format_.width ||
                          frame.height != video_format_.height)) {
        if (scaler_src_width_ != frame.width || scaler_src_height_ != frame.height) {
            scaler_ = ColorConverter{};
            if (auto s = scaler_.Init(device_, frame.width, frame.height, frame.format,
                                      video_format_.width, video_format_.height,
                                      DXGI_FORMAT_B8G8R8A8_UNORM, ColorSpace::Rec709);
                !s.ok()) {
                RF_WARN("cannot scale this display into the recording: {}", s.str());
                return;
            }
            scaler_src_width_ = frame.width;
            scaler_src_height_ = frame.height;
            RF_INFO("scaling {}x{} into the {}x{} recording", frame.width, frame.height,
                    video_format_.width, video_format_.height);
        }

        ID3D11Texture2D* scaled = nullptr;
        if (auto s = scaler_.Convert(frame.texture, &scaled); !s.ok()) {
            RF_WARN("display scale failed: {}", s.str());
            return;
        }
        fitted.texture = scaled;
        fitted.width = video_format_.width;
        fitted.height = video_format_.height;
        fitted.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    const Ticks100ns period = pacer_period_ > 0
                                  ? pacer_period_
                                  : kOneSecond100ns * video_format_.fps_den /
                                        std::max(1u, video_format_.fps_num);

    std::scoped_lock lock(pace_mutex_);
    SubmitPacedLocked(fitted, period);
    if (frame_event_) ::SetEvent(frame_event_);
}

void Recorder::StartPacer() {
    if (pacing_) return;
    if (!frame_event_) frame_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    pacing_ = true;
    paced_index_ = 0;
    pacer_ = std::thread([this] { PacerLoop(); });
}

void Recorder::StopPacer() {
    pacing_ = false;
    if (frame_event_) ::SetEvent(frame_event_);
    if (pacer_.joinable()) pacer_.join();
    std::scoped_lock lock(pace_mutex_);
    have_last_ = false;
}

void Recorder::PacerLoop() {
    ::SetThreadDescription(::GetCurrentThread(), L"rf-idle-fill");
    MmcssScope mmcss(L"Capture");

    const Ticks100ns period = pacer_period_ > 0
                                  ? pacer_period_
                                  : kOneSecond100ns * video_format_.fps_den /
                                        std::max(1u, video_format_.fps_num);
    const DWORD wait_ms = static_cast<DWORD>(std::max<Ticks100ns>(1, period / 10'000));

    (void)wait_ms;

    const DWORD idle_wait =
        static_cast<DWORD>(std::clamp<Ticks100ns>(2 * period / 10'000, 8, 40));
    const std::int64_t stall_slots =
        std::max<std::int64_t>(8, (kOneSecond100ns / 4) / period);

    Ticks100ns last_stats = Now100ns();
    std::uint64_t last_encoded = 0, last_dropped = 0;

    while (pacing_.load(std::memory_order_relaxed)) {
        ::WaitForSingleObject(frame_event_, idle_wait);
        if (!pacing_.load(std::memory_order_relaxed)) break;
        if (!encoder_ready_.load(std::memory_order_acquire)) continue;

        if (const Ticks100ns now_stats = Now100ns();
            now_stats - last_stats > 10 * kOneSecond100ns) {
            const EncoderStats es = encoder_->stats();
            const double secs = static_cast<double>(now_stats - last_stats) / kOneSecond100ns;
            RF_INFO("encoder: {:.1f} fps out, {} dropped in the last {:.0f}s (queue {:.1f}, {} total)",
                    (es.frames_encoded - last_encoded) / secs, es.frames_dropped - last_dropped,
                    secs, es.avg_queue_depth, es.frames_encoded);
            last_stats = now_stats;
            last_encoded = es.frames_encoded;
            last_dropped = es.frames_dropped;
        }

        std::scoped_lock lock(pace_mutex_);
        if (!have_last_) continue;

        const Ticks100ns now = Now100ns();

        if (now - last_pts_ > stall_slots * period) last_pts_ = now - period;

        for (int filled = 0; now - last_pts_ > period * 3 / 2 && filled < 8; ++filled) {
            last_pts_ += period;
            CapturedFrame repeat;
            repeat.texture = nullptr;
            repeat.width = video_format_.width;
            repeat.height = video_format_.height;
            repeat.timestamp = last_pts_;
            repeat.frame_index = paced_index_++;
            repeat.content_changed = false;
            if (auto s = encoder_->Submit(repeat); !s.ok())
                RF_WARN("encoder submit: {}", s.str());
            next_deadline_ = last_pts_ + period;
        }
    }
}

void Recorder::OnPacket(PacketPtr packet) {
    if (!packet) return;

    if (settings_.replay_enabled) replay_.Push(packet);

    if (state_.load() == State::Recording) {
        std::scoped_lock lock(mux_mutex_);
        if (muxer_)
            if (auto s = muxer_->WritePacket(*packet); !s.ok()) RF_WARN("mux: {}", s.str());
    }
}

std::filesystem::path Recorder::MakeOutputPath(const char* suffix) const {
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    const auto stamp = std::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", now.wYear, now.wMonth,
                                   now.wDay, now.wHour, now.wMinute, now.wSecond);
    return settings_.output_dir / ToWide(std::format("Reframe_{}_{}.mp4", suffix, stamp));
}

Status Recorder::SaveReplay(std::filesystem::path* saved_to, std::uint32_t seconds) {

    std::scoped_lock pipeline_lock(pipeline_mutex_);
    if (!settings_.replay_enabled) return Status::Fail("instant replay is disabled");

    const Ticks100ns window = seconds ? static_cast<Ticks100ns>(seconds) * kOneSecond100ns
                                      : static_cast<Ticks100ns>(settings_.replay_seconds) *
                                            kOneSecond100ns;

    const auto packets = replay_.Snapshot(window);
    if (packets.empty()) return Status::Fail("replay buffer is empty");

    const auto path = MakeOutputPath("Replay");

    Mp4Muxer muxer;
    RF_TRY(muxer.Open(path, video_format_, encoder_ ? encoder_->codec_private() : CodecPrivate{},
                      aac_ ? aac_->output_type() : nullptr, aac_mic_ ? 2 : 1));
    for (const auto& p : packets) RF_TRY(muxer.WritePacket(*p));
    RF_TRY(muxer.Close());

    if (saved_to) *saved_to = path;

    replay_.Clear();
    if (encoder_) encoder_->RequestKeyframe();

    RF_INFO("replay saved: {}", path.string());
    return Status::Ok();
}

Status Recorder::StartRecording(const std::filesystem::path& file) {
    std::scoped_lock pipeline_lock(pipeline_mutex_);
    if (state_ == State::Idle) RF_TRY(ArmReplayLocked());
    if (state_ == State::Recording) return Status::Fail("already recording");

    if (!encoder_) return Status::Fail("нечего записывать - нет подходящего окна");

    auto muxer = std::make_unique<Mp4Muxer>();
    RF_TRY(muxer->Open(file.empty() ? MakeOutputPath("Recording") : file, video_format_,
                       encoder_->codec_private(), aac_ ? aac_->output_type() : nullptr,
                       aac_mic_ ? 2 : 1));

    encoder_->RequestKeyframe();

    {
        std::scoped_lock lock(mux_mutex_);
        muxer_ = std::move(muxer);
    }
    state_ = State::Recording;
    return Status::Ok();
}

Status Recorder::StopRecording() {
    std::scoped_lock pipeline_lock(pipeline_mutex_);
    if (state_ != State::Recording) return Status::Fail("not recording");
    state_ = State::ReplayArmed;

    if (encoder_) encoder_->Flush();

    std::scoped_lock lock(mux_mutex_);
    if (muxer_) {
        RF_TRY(muxer_->Close());
        muxer_.reset();
    }
    return Status::Ok();
}

std::string Recorder::target_app() const {
    if (!settings_.capture_focused_window_only) return {};

    std::string exe = game_watcher_.current().exe;
    if (exe.empty()) return {};

    if (exe.size() > 4 && exe.compare(exe.size() - 4, 4, ".exe") == 0) exe.resize(exe.size() - 4);
    if (!exe.empty() && exe[0] >= 'a' && exe[0] <= 'z') exe[0] -= 'a' - 'A';
    return exe;
}

Status Recorder::SetCaptureMonitor(void* hmonitor) {
    std::scoped_lock lock(pipeline_mutex_);
    return SetCaptureMonitorLocked(hmonitor);
}

Status Recorder::SetCaptureMonitorLocked(void* hmonitor) {
    if (!hmonitor || hmonitor == active_monitor_) return Status::Ok();

    void* previous = active_monitor_;
    active_monitor_ = hmonitor;

    if (state_ == State::Idle || !capture_ || settings_.capture_focused_window_only)
        return Status::Ok();

    CaptureTarget target;
    target.kind = CaptureTarget::Kind::Display;
    target.hmonitor = hmonitor;
    target.capture_cursor = settings_.capture_cursor;
    target.fps = settings_.ResolvedFps();

    auto on_frame = [this](const CapturedFrame& f) { OnFrame(f); };

    VideoCapturePtr fresh;
    if (Status s = StartDisplayCapture(target, on_frame, fresh); !s) {
        RF_WARN("could not switch to the other display ({}) - staying on the current one",
                s.message());
        active_monitor_ = previous;
        return s;
    }

    capture_->Stop();
    capture_ = std::move(fresh);
    RF_INFO("recording moved to another display ({}x{})", capture_->width(), capture_->height());
    return Status::Ok();
}

bool Recorder::capture_is_hooked() const {
    return (capture_ && capture_->backend() == CaptureBackend::GameHook) ||
           (overlay_hook_ && overlay_hook_->attached());
}

void Recorder::SetInGameOverlay(std::uint32_t handle, std::uint32_t serial, std::uint32_t width,
                                std::uint32_t height, bool visible) {
    std::scoped_lock lock(pipeline_mutex_);
    if (capture_ && capture_->backend() == CaptureBackend::GameHook)
        static_cast<GameCapture*>(capture_.get())->SetOverlay(handle, serial, width, height, visible);
    else if (overlay_hook_ && overlay_hook_->attached())
        overlay_hook_->SetOverlay(handle, serial, width, height, visible);
}

Recorder::Status_ Recorder::GetStatus() const {
    Status_ s;
    s.state = state_.load();
    if (capture_) {
        s.capture = capture_->stats();
        s.capture_backend = ToString(capture_->backend());
    }
    if (encoder_) {
        s.encode = encoder_->stats();
        s.encoder_backend = encoder_->name();
    }
    s.replay = replay_.GetStats();
    if (device_) s.gpu = ToUtf8(device_->info().description);
    s.quirks = quirks_.describe();
    return s;
}

void Recorder::Shutdown() {

    game_watcher_.Stop();

    if (state_ == State::Recording) StopRecording();
    DisarmReplay();
    if (frame_event_) {
        ::CloseHandle(frame_event_);
        frame_event_ = nullptr;
    }
    device_.reset();
    if (mf_started_) {
        MFShutdown();
        mf_started_ = false;
    }
}

}
