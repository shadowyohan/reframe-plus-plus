#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <numeric>
#include <string>
#include <vector>

#include "rf/mux/ClipCropper.h"

#include "test_framework.h"

using Microsoft::WRL::ComPtr;
using namespace rf;

namespace {

constexpr std::uint32_t kWidth = 320;
constexpr std::uint32_t kHeight = 232;
constexpr std::uint8_t kGameChroma = 180;
constexpr std::uint32_t kFps = 30;
constexpr std::uint32_t kFrames = 30;
constexpr Ticks100ns kFrameTime = kOneSecond100ns / kFps;
constexpr PixelRect kGameArea{40, 20, 200, 140};

ComPtr<IMFMediaType> Video(const GUID& subtype) {
    ComPtr<IMFMediaType> type;
    ::MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, subtype);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    ::MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, kWidth, kHeight);
    ::MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, kFps, 1);
    ::MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    return type;
}

bool WriteSyntheticClip(const std::filesystem::path& file) {
    ComPtr<IMFAttributes> attributes;
    ::MFCreateAttributes(&attributes, 1);
    attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    ComPtr<IMFSinkWriter> writer;
    if (FAILED(::MFCreateSinkWriterFromURL(file.c_str(), nullptr, attributes.Get(), &writer)))
        return false;

    ComPtr<IMFMediaType> encoded = Video(MFVideoFormat_H264);
    encoded->SetUINT32(MF_MT_AVG_BITRATE, 2'000'000);
    DWORD stream = 0;
    if (FAILED(writer->AddStream(encoded.Get(), &stream)) ||
        FAILED(writer->SetInputMediaType(stream, Video(MFVideoFormat_NV12).Get(), nullptr)) ||
        FAILED(writer->BeginWriting()))
        return false;

    const auto in_game = [](std::uint32_t x, std::uint32_t y) {
        return static_cast<std::int32_t>(x) >= kGameArea.left &&
               static_cast<std::int32_t>(x) < kGameArea.right &&
               static_cast<std::int32_t>(y) >= kGameArea.top &&
               static_cast<std::int32_t>(y) < kGameArea.bottom;
    };
    std::vector<std::uint8_t> frame(kWidth * kHeight * 3 / 2, 128);
    for (std::uint32_t y = 0; y < kHeight; ++y)
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            frame[y * kWidth + x] = in_game(x, y) ? 100 : 220;
            if (y % 2 == 0 && in_game(x, y)) frame[kWidth * kHeight + y / 2 * kWidth + x] = kGameChroma;
        }

    for (std::uint32_t i = 0; i < kFrames; ++i) {
        ComPtr<IMFMediaBuffer> buffer;
        ::MFCreateMemoryBuffer(static_cast<DWORD>(frame.size()), &buffer);
        BYTE* data = nullptr;
        buffer->Lock(&data, nullptr, nullptr);
        std::memcpy(data, frame.data(), frame.size());
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(frame.size()));
        ComPtr<IMFSample> sample;
        ::MFCreateSample(&sample);
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(i * kFrameTime);
        sample->SetSampleDuration(kFrameTime);
        if (FAILED(writer->WriteSample(stream, sample.Get()))) return false;
    }
    return SUCCEEDED(writer->Finalize());
}

struct DecodedFrame {
    LONGLONG time = 0;
    double mean_luma = 0;
    double mean_chroma = 0;
};

bool ReadBack(const std::filesystem::path& file, UINT32& width, UINT32& height,
              std::vector<DecodedFrame>& frames) {
    ComPtr<IMFAttributes> attributes;
    ::MFCreateAttributes(&attributes, 1);
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> reader;
    if (FAILED(::MFCreateSourceReaderFromURL(file.c_str(), attributes.Get(), &reader))) return false;

    ComPtr<IMFMediaType> nv12;
    ::MFCreateMediaType(&nv12);
    nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    const auto video = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    if (FAILED(reader->SetCurrentMediaType(video, nullptr, nv12.Get()))) return false;

    ComPtr<IMFMediaType> native;
    reader->GetNativeMediaType(video, 0, &native);
    ::MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &width, &height);

    while (true) {
        DWORD flags = 0;
        LONGLONG time = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(video, 0, nullptr, &flags, &time, &sample))) return false;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return true;
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buffer;
        sample->ConvertToContiguousBuffer(&buffer);
        BYTE* data = nullptr;
        DWORD length = 0;
        buffer->Lock(&data, nullptr, &length);
        const std::size_t luma = std::min<std::size_t>(length, std::size_t{width} * height);
        const double luma_sum = std::accumulate(data, data + luma, 0.0);
        const std::size_t chroma_start = std::size_t{length} * 2 / 3;
        const std::size_t chroma = std::min<std::size_t>(length - chroma_start, luma / 2);
        const double chroma_sum =
            std::accumulate(data + chroma_start, data + chroma_start + chroma, 0.0);
        buffer->Unlock();
        frames.push_back({time, luma ? luma_sum / luma : 0, chroma ? chroma_sum / chroma : 0});
    }
}

}

TEST(ClipCropper_FullscreenVisibleClipNeedsNothing) {
    const std::vector<WindowSample> fullscreen{{0, true, {0, 0, 1920, 1080}},
                                               {kOneSecond100ns, true, {0, 0, 1920, 1080}}};
    CHECK(!NeedsCropping(fullscreen, 1920, 1080));
    CHECK(!NeedsCropping({}, 1920, 1080));

    const std::vector<WindowSample> windowed{{0, true, {100, 50, 1380, 770}}};
    CHECK(NeedsCropping(windowed, 1920, 1080));
    const std::vector<WindowSample> minimized{{0, false, {0, 0, 1920, 1080}}};
    CHECK(NeedsCropping(minimized, 1920, 1080));
}

TEST(ClipCropper_OutputIsTheLastVisibleWindowRoundedToEven) {
    const std::vector<WindowSample> samples{{0, true, {10, 10, 811, 611}},
                                            {kOneSecond100ns, true, {5, 5, 1285, 725}},
                                            {2 * kOneSecond100ns, false, {}}};
    const PixelRect size = CropOutputSize(samples, 1920, 1080);
    CHECK_EQ(size.width(), 1280);
    CHECK_EQ(size.height(), 720);
}

TEST(ClipCropper_FramesFollowTheSampleInForce) {
    const std::vector<WindowSample> samples{{0, true, {11, 21, 111, 121}},
                                            {kOneSecond100ns, false, {}}};
    const FramePlan early = PlanFrame(samples, kOneSecond100ns / 2, 1920, 1080);
    CHECK(!early.black);
    CHECK_EQ(early.source.left, 10);
    CHECK_EQ(early.source.top, 20);
    CHECK(PlanFrame(samples, kOneSecond100ns, 1920, 1080).black);
    CHECK(!PlanFrame(samples, -kOneSecond100ns, 1920, 1080).black);
}

TEST(ClipCropper_CopiesTheWindowAndBlanksTheRest) {
    constexpr std::uint32_t kPitch = 8, kRows = 4;
    std::vector<std::uint8_t> source(kPitch * kRows * 3 / 2);
    std::iota(source.begin(), source.end(), std::uint8_t{0});

    std::vector<std::uint8_t> target(4 * 2 * 3 / 2);
    CopyNv12(source.data(), kPitch, kRows, {false, {2, 2, 6, 4}}, target.data(), 4, 2);
    CHECK_EQ(target[0], source[2 * kPitch + 2]);
    CHECK_EQ(target[4], source[3 * kPitch + 2]);
    CHECK_EQ(target[8], source[kPitch * kRows + 1 * kPitch + 2]);

    CopyNv12(source.data(), kPitch, kRows, {true, {}}, target.data(), 4, 2);
    CHECK_EQ(target[0], 16);
    CHECK_EQ(target[8], 128);
}

static void CropSyntheticClip(bool use_gpu) {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(::MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        SKIP("Media Foundation unavailable");
        return;
    }

    const auto dir = std::filesystem::temp_directory_path() / "reframe-cropper-test";
    std::filesystem::create_directories(dir);
    CropJob job;
    job.raw = dir / "clip.mp4.part";
    job.output = dir / "clip.mp4";
    job.frame_width = kWidth;
    job.frame_height = kHeight;
    job.bitrate_kbps = 2000;
    constexpr Ticks100ns kShown = kOneSecond100ns / 4;
    constexpr Ticks100ns kHidden = kOneSecond100ns * 3 / 4;
    job.samples = {{0, false, {}}, {kShown, true, kGameArea}, {kHidden, false, {}}};
    job.use_gpu = use_gpu;

    if (!WriteSyntheticClip(job.raw)) {
        SKIP("no H.264 encoder");
    } else {
        const Status cropped = CropClip(job);
        CHECK(cropped.ok());
        CHECK(!std::filesystem::exists(job.raw));

        UINT32 width = 0, height = 0;
        std::vector<DecodedFrame> frames;
        CHECK(ReadBack(job.output, width, height, frames));
        CHECK_EQ(width, static_cast<UINT32>(kGameArea.width()));
        CHECK_EQ(height, static_cast<UINT32>(kGameArea.height()));
        CHECK(frames.size() >= kFrames - 2);
        for (const DecodedFrame& frame : frames) {
            const bool hidden = frame.time < kShown - kFrameTime || frame.time > kHidden + kFrameTime;
            const bool shown = frame.time > kShown + kFrameTime && frame.time < kHidden - kFrameTime;
            if (shown) {
                CHECK(frame.mean_luma > 90 && frame.mean_luma < 110);
                CHECK(frame.mean_chroma > kGameChroma - 10 && frame.mean_chroma < kGameChroma + 10);
            }
            if (hidden) CHECK(frame.mean_luma < 24);
        }
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    ::MFShutdown();
    ::CoUninitialize();
}

TEST(ClipCropper_JobSurvivesTheTripToTheHelper) {
    CropJob job;
    job.raw = std::filesystem::temp_directory_path() / L"Клип игры.mp4.part";
    job.output = std::filesystem::temp_directory_path() / L"Клип игры.mp4";
    job.frame_width = 1920;
    job.frame_height = 1080;
    job.bitrate_kbps = 15000;
    job.audio_names = {"Звук системы", "Моя Игра"};
    job.first_track_is_full_mix = true;
    job.use_gpu = false;
    job.samples = {{0, false, {}}, {5'000'000, true, {10, 20, 1290, 740}}};

    const auto file = std::filesystem::temp_directory_path() / "reframe-crop-job-test.job";
    CHECK(WriteCropJob(file, job).ok());
    CropJob read;
    CHECK(ReadCropJob(file, read).ok());
    std::filesystem::remove(file);

    CHECK(read.raw == job.raw);
    CHECK(read.output == job.output);
    CHECK(read.audio_names == job.audio_names);
    CHECK(read.first_track_is_full_mix);
    CHECK(!read.use_gpu);
    CHECK_EQ(read.bitrate_kbps, 15000u);
    CHECK_EQ(read.samples.size(), std::size_t{2});
    CHECK(read.samples[1].at == 5'000'000 && read.samples[1].visible);
    CHECK(read.samples[1].crop == (PixelRect{10, 20, 1290, 740}));
}

TEST(ClipCropper_HelperProcessCropsAndReplacesTheClip) {
    wchar_t self[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    const auto helper = std::filesystem::path(self).parent_path() / "Reframe.exe";
    if (!std::filesystem::exists(helper)) {
        SKIP("Reframe.exe is not built next to the tests");
        return;
    }
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ::MFStartup(MF_VERSION, MFSTARTUP_LITE);

    const auto dir = std::filesystem::temp_directory_path() / "reframe-cropper-helper-test";
    std::filesystem::create_directories(dir);
    CropJob job;
    job.raw = dir / "clip.mp4.part";
    job.output = dir / "clip.mp4";
    job.frame_width = kWidth;
    job.frame_height = kHeight;
    job.bitrate_kbps = 2000;
    job.samples = {{0, true, kGameArea}};

    if (!WriteSyntheticClip(job.raw)) {
        SKIP("no H.264 encoder");
    } else {
        CHECK(CropInHelperProcess(job, helper).ok());
        CHECK(!std::filesystem::exists(job.raw));
        UINT32 width = 0, height = 0;
        std::vector<DecodedFrame> frames;
        CHECK(ReadBack(job.output, width, height, frames));
        CHECK_EQ(width, static_cast<UINT32>(kGameArea.width()));
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    ::MFShutdown();
    ::CoUninitialize();
}

TEST(ClipCropper_CropsARealClipOnTheGpu) { CropSyntheticClip(true); }

TEST(ClipCropper_CropsARealClipOnTheCpu) { CropSyntheticClip(false); }

namespace {

std::string Run(const std::string& command) {
    std::string output;
    if (std::FILE* pipe = _popen(command.c_str(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof(line), pipe)) output += line;
        _pclose(pipe);
    }
    return output;
}

double MediaFoundationDuration(const std::filesystem::path& file) {
    ComPtr<IMFSourceReader> reader;
    if (FAILED(::MFCreateSourceReaderFromURL(file.c_str(), nullptr, &reader))) return 0.0;
    PROPVARIANT value;
    ::PropVariantInit(&value);
    double seconds = 0.0;
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                                                   MF_PD_DURATION, &value)) &&
        value.vt == VT_UI8)
        seconds = static_cast<double>(value.uhVal.QuadPart) / 1e7;
    ::PropVariantClear(&value);
    return seconds;
}

double MaxVolume(const std::filesystem::path& file, int track) {
    const std::string out = Run(std::format("ffmpeg -v info -i \"{}\" -map 0:a:{} -af volumedetect -f null - 2>&1",
                                            file.string(), track));
    const auto at = out.find("max_volume:");
    return at == std::string::npos ? 0.0 : std::atof(out.c_str() + at + 11);
}

}

TEST(ClipCropper_EditorTrimsDropsAndQuietensTracks) {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
    const auto dir = std::filesystem::temp_directory_path() / "reframe-editor-test";
    std::filesystem::create_directories(dir);
    const auto source = dir / "clip.mp4";
    const std::string make = std::format(
        "ffmpeg -v error -y -f lavfi -i testsrc=duration=4:size=320x240:rate=30 "
        "-f lavfi -i sine=frequency=440:duration=4 -f lavfi -i sine=frequency=880:duration=4 "
        "-filter_complex [2:a]volume=0.25[quiet] -map 0 -map 1 -map [quiet] -c:v libx264 -g 30 -pix_fmt yuv420p -c:a aac -ar 48000 -ac 2 \"{}\" 2>nul",
        source.string());
    if (std::system(make.c_str()) != 0 || !std::filesystem::exists(source)) {
        SKIP("ffmpeg with libx264 is not installed");
    } else {
        CropJob job;
        job.raw = source;
        job.output = dir / "clip_edit.mp4";
        job.frame_width = 320;
        job.frame_height = 240;
        job.bitrate_kbps = 2000;
        job.trim_start = kOneSecond100ns;
        job.trim_end = kOneSecond100ns * 3;
        job.audio_edits = {{true, 0.5f}, {false, 1.0f}};
        job.audio_names = {"Mix"};
        job.keep_raw = true;

        const Status edited = CropClip(job);
        if (!edited.ok()) std::printf("    %s\n", edited.str().c_str());
        CHECK(edited.ok());
        CHECK(std::filesystem::exists(source));

        const std::string streams =
            Run(std::format("ffprobe -v error -show_entries stream=codec_type,duration -of csv=p=0 \"{}\"",
                            job.output.string()));
        CHECK(streams.find("audio") == streams.rfind("audio"));
        const double duration =
            std::atof(Run(std::format("ffprobe -v error -show_entries format=duration -of csv=p=0 \"{}\"",
                                      job.output.string()))
                          .c_str());
        CHECK(duration > 1.8 && duration < 2.3);
        const double player_duration = MediaFoundationDuration(job.output);
        CHECK(player_duration > 1.8 && player_duration < 2.3);
        const double quieter = MaxVolume(source, 0) - MaxVolume(job.output, 0);
        CHECK(quieter > 5.0 && quieter < 7.0);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    ::MFShutdown();
    ::CoUninitialize();
}

TEST(ClipCropper_EditorSpeedOnARealClip) {
    char source_path[MAX_PATH] = {};
    if (::GetEnvironmentVariableA("RF_EDIT_BENCH", source_path, sizeof(source_path)) == 0) {
        SKIP("set RF_EDIT_BENCH=<clip.mp4> to time a real edit");
        return;
    }
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
    const auto dir = std::filesystem::temp_directory_path() / "reframe-editor-bench";
    std::filesystem::create_directories(dir);

    {
        CropJob job;
        job.raw = source_path;
        job.output = dir / "audio.mp4";
        job.frame_width = 1920;
        job.frame_height = 1080;
        job.audio_edits = {{true, 0.5f}};
        job.keep_raw = true;
        const auto started = ::GetTickCount64();
        const Status edited = CropClip(job);
        std::printf("    audio-only edit of the whole clip in %.1f s (%s)\n",
                    (::GetTickCount64() - started) / 1000.0, edited.ok() ? "ok" : edited.str().c_str());
    }

    for (const bool gpu : {true, false}) {
        CropJob job;
        job.raw = source_path;
        job.output = dir / (gpu ? "gpu.mp4" : "cpu.mp4");
        job.frame_width = 1920;
        job.frame_height = 1080;
        job.bitrate_kbps = 15000;
        job.trim_start = kOneSecond100ns * 5;
        job.trim_end = kOneSecond100ns * 15;
        job.keep_raw = true;
        job.use_gpu = gpu;
        const auto started = ::GetTickCount64();
        const Status edited = CropClip(job);
        std::printf("    %s: 10 s of 1080p in %.1f s (%s)\n", gpu ? "gpu" : "cpu",
                    (::GetTickCount64() - started) / 1000.0, edited.ok() ? "ok" : edited.str().c_str());
    }

    char keep[8] = {};
    if (::GetEnvironmentVariableA("RF_EDIT_BENCH_KEEP", keep, sizeof(keep)) > 0) return;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    ::MFShutdown();
    ::CoUninitialize();
}
