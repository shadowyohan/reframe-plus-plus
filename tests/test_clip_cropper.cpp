#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstring>
#include <filesystem>
#include <numeric>
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
