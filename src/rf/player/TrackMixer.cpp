#include "rf/player/TrackMixer.h"

#include <windows.h>

#include <audioclient.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>

#include "rf/core/Log.h"

using Microsoft::WRL::ComPtr;

namespace rf {
namespace {

constexpr UINT32 kRate = 48'000;
constexpr UINT32 kChannels = 2;
constexpr REFERENCE_TIME kBufferDuration = 60 * 10'000;
constexpr DWORD kWakeMs = 100;

}

struct TrackMixer::Track {
    ComPtr<IMFSourceReader> reader;
    DWORD stream = 0;
    std::vector<float> fifo;
    std::size_t read_at = 0;
    LONGLONG skip_until = -1;
    bool ended = false;

    [[nodiscard]] std::size_t buffered() const { return fifo.size() - read_at; }
};

TrackMixer::~TrackMixer() { Close(); }

void TrackMixer::Open(const std::filesystem::path& file, std::size_t tracks) {
    Close();
    file_ = file;
    track_count_ = tracks;
    gains_ = std::make_unique<std::atomic<float>[]>(tracks);
    for (std::size_t i = 0; i < tracks; ++i) gains_[i] = 1.0f;
    playing_ = false;
    seek_to_ = 0.0;
    position_ = 0.0;
    running_ = true;
    thread_ = std::thread([this] { Loop(); });
}

void TrackMixer::Close() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void TrackMixer::SetGain(std::size_t track, float gain) {
    if (track < track_count_) gains_[track].store(std::clamp(gain, 0.0f, kMaxGain));
}

void TrackMixer::Play(double from_seconds) {
    seek_to_ = std::max(0.0, from_seconds);
    playing_ = true;
}

void TrackMixer::Pause() { playing_ = false; }

void TrackMixer::Seek(double seconds) { seek_to_ = std::max(0.0, seconds); }

Status TrackMixer::OpenTracks(std::vector<Track>& tracks) const {
    for (std::size_t wanted = 0; wanted < track_count_; ++wanted) {
        Track track;
        RF_HR(::MFCreateSourceReaderFromURL(file_.c_str(), nullptr, &track.reader));
        RF_HR(track.reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));

        std::size_t audio_seen = 0;
        bool found = false;
        for (DWORD index = 0; !found; ++index) {
            ComPtr<IMFMediaType> native;
            if (FAILED(track.reader->GetNativeMediaType(index, 0, &native))) break;
            GUID major{};
            if (FAILED(native->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Audio) continue;
            if (audio_seen++ != wanted) continue;
            track.stream = index;
            found = true;
        }
        if (!found) return Status::Fail("the clip has fewer audio tracks than expected");

        ComPtr<IMFMediaType> pcm;
        RF_HR(::MFCreateMediaType(&pcm));
        RF_HR(pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
        RF_HR(pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float));
        RF_HR(pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels));
        RF_HR(pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate));
        RF_HR(pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32));
        RF_HR(pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kChannels * 4));
        RF_HR(pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * kChannels * 4));
        RF_HR(track.reader->SetStreamSelection(track.stream, TRUE));
        RF_HR(track.reader->SetCurrentMediaType(track.stream, nullptr, pcm.Get()));
        tracks.push_back(std::move(track));
    }
    return Status::Ok();
}

void TrackMixer::SeekTrack(Track& track, double seconds) {
    const auto target = static_cast<LONGLONG>(seconds * 10'000'000.0);
    PROPVARIANT position;
    ::InitPropVariantFromInt64(target, &position);
    track.reader->SetCurrentPosition(GUID_NULL, position);
    ::PropVariantClear(&position);
    track.fifo.clear();
    track.read_at = 0;
    track.skip_until = target;
    track.ended = false;
}

void TrackMixer::Fill(Track& track, std::size_t samples) {
    while (!track.ended && track.buffered() < samples) {
        DWORD flags = 0;
        LONGLONG time = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(track.reader->ReadSample(track.stream, 0, nullptr, &flags, &time, &sample)) ||
            (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))) {
            track.ended = true;
            break;
        }
        if (!sample) continue;

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &length))) continue;
        const auto* values = reinterpret_cast<const float*>(data);
        std::size_t count = length / sizeof(float);

        std::size_t skip = 0;
        if (track.skip_until >= 0) {
            if (time < track.skip_until)
                skip = static_cast<std::size_t>((track.skip_until - time) * kRate / 10'000'000) * kChannels;
            track.skip_until = -1;
        }
        if (skip < count) track.fifo.insert(track.fifo.end(), values + skip, values + count);
        buffer->Unlock();
    }

    if (track.read_at > 1u << 18) {
        track.fifo.erase(track.fifo.begin(), track.fifo.begin() + static_cast<std::ptrdiff_t>(track.read_at));
        track.read_at = 0;
    }
}

void TrackMixer::Loop() {
    ::SetThreadDescription(::GetCurrentThread(), L"rf-editor-audio");
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool media = SUCCEEDED(::MFStartup(MF_VERSION, MFSTARTUP_LITE));

    std::vector<Track> tracks;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    HANDLE event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT32 buffer_frames = 0;

    const Status ready = [&]() -> Status {
        RF_TRY(OpenTracks(tracks));
        RF_HR(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 IID_PPV_ARGS(&enumerator)));
        RF_HR(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device));
        RF_HR(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client));
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = kChannels;
        format.nSamplesPerSec = kRate;
        format.wBitsPerSample = 32;
        format.nBlockAlign = kChannels * 4;
        format.nAvgBytesPerSec = kRate * format.nBlockAlign;
        RF_HR(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                     AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 kBufferDuration, 0, &format, nullptr));
        RF_HR(client->SetEventHandle(event));
        RF_HR(client->GetBufferSize(&buffer_frames));
        RF_HR(client->GetService(IID_PPV_ARGS(&render)));
        RF_HR(client->Start());
        return Status::Ok();
    }();
    if (!ready.ok()) RF_WARN("editor audio preview unavailable: {}", ready.str());

    double written_seconds = 0.0;
    std::vector<float> mix;
    while (ready.ok() && running_.load(std::memory_order_relaxed)) {
        ::WaitForSingleObject(event, kWakeMs);

        if (const double target = seek_to_.exchange(-1.0); target >= 0.0) {
            for (Track& track : tracks) SeekTrack(track, target);
            written_seconds = target;
        }

        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) break;
        const UINT32 frames = buffer_frames - padding;
        position_ = std::max(0.0, written_seconds - static_cast<double>(padding) / kRate);
        if (frames == 0) continue;

        BYTE* out = nullptr;
        if (FAILED(render->GetBuffer(frames, &out))) break;
        if (!playing_.load(std::memory_order_relaxed)) {
            render->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
            continue;
        }

        const std::size_t samples = static_cast<std::size_t>(frames) * kChannels;
        mix.assign(samples, 0.0f);
        for (std::size_t t = 0; t < tracks.size(); ++t) {
            Track& track = tracks[t];
            Fill(track, samples);
            const float gain = gains_[t].load(std::memory_order_relaxed);
            const std::size_t available = std::min(samples, track.buffered());
            for (std::size_t i = 0; i < available; ++i) mix[i] += track.fifo[track.read_at + i] * gain;
            track.read_at += available;
        }
        const float master = master_.load(std::memory_order_relaxed);
        auto* target = reinterpret_cast<float*>(out);
        for (std::size_t i = 0; i < samples; ++i) target[i] = std::clamp(mix[i] * master, -1.0f, 1.0f);
        render->ReleaseBuffer(frames, 0);
        written_seconds += static_cast<double>(frames) / kRate;
    }

    if (client) client->Stop();
    render.Reset();
    client.Reset();
    tracks.clear();
    ::CloseHandle(event);
    if (media) ::MFShutdown();
    ::CoUninitialize();
}

}
