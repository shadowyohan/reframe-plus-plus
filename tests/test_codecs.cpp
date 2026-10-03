#include <windows.h>

#include <mfapi.h>

#include <cstdio>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "rf/encode/NvencEncoder.h"
#include "rf/gpu/D3DDevice.h"
#include "rf/gpu/GpuInfo.h"
#include "rf/mux/Mp4Muxer.h"

#include "test_framework.h"

using namespace rf;

namespace {

std::string Probe(const std::filesystem::path& file) {
    std::string output;
    const std::string command = std::format(
        "ffprobe -v error -select_streams v -show_entries stream=codec_name,codec_tag_string,width,height,nb_frames "
        "-of csv=p=0 \"{}\" 2>nul",
        file.string());
    if (std::FILE* pipe = _popen(command.c_str(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), pipe)) output += line;
        _pclose(pipe);
    }
    return output;
}

std::string EncodeWithNvenc(Codec codec, const std::filesystem::path& file) {
    D3DDevicePtr device;
    for (const AdapterInfo& adapter : EnumerateAdapters())
        if (adapter.vendor == GpuVendor::Nvidia && D3DDevice::Create(adapter, device).ok()) break;
    if (!device) return "no NVIDIA card";

    constexpr std::uint32_t kWidth = 640, kHeight = 360, kFrames = 60;
    EncoderConfig config;
    config.format = {kWidth, kHeight, 30, 1, codec, ColorSpace::Rec709};
    config.bitrate_kbps = 4000;
    config.input_width = kWidth;
    config.input_height = kHeight;

    std::vector<PacketPtr> packets;
    NvencEncoder encoder(device);
    if (Status s = encoder.Open(config, [&](PacketPtr p) { packets.push_back(std::move(p)); }); !s.ok())
        return s.str();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    std::vector<std::uint32_t> pixels(kWidth * kHeight);
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        for (std::uint32_t p = 0; p < pixels.size(); ++p)
            pixels[p] = 0xFF000000u | ((p % kWidth + i * 8) & 0xFF) << 16 | ((p / kWidth) & 0xFF) << 8;
        const D3D11_SUBRESOURCE_DATA data{pixels.data(), kWidth * 4, 0};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        device->device()->CreateTexture2D(&desc, &data, &texture);
        CapturedFrame frame;
        frame.texture = texture.Get();
        frame.width = kWidth;
        frame.height = kHeight;
        frame.timestamp = static_cast<Ticks100ns>(i) * kOneSecond100ns / 30;
        frame.frame_index = i;
        if (Status s = encoder.Submit(frame); !s.ok()) return s.str();
    }
    encoder.Flush();
    encoder.Close();
    if (packets.empty()) return "no packets";

    Mp4Muxer muxer;
    if (Status s = muxer.Open(file, config.format, encoder.codec_private(), nullptr, 0); !s.ok()) return s.str();
    for (const PacketPtr& packet : packets)
        if (Status s = muxer.WritePacket(*packet); !s.ok()) return s.str();
    if (Status s = muxer.Close(); !s.ok()) return s.str();
    return {};
}

}

TEST(Codecs_NvencWritesPlayableHevc) {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
    const auto file = std::filesystem::temp_directory_path() / "reframe-hevc-test.mp4";
    const std::string failure = EncodeWithNvenc(Codec::HEVC, file);
    if (failure == "no NVIDIA card") {
        SKIP("no NVIDIA card");
    } else {
        if (!failure.empty()) std::printf("    %s\n", failure.c_str());
        CHECK(failure.empty());
        const std::string probe = Probe(file);
        std::printf("    %s", probe.c_str());
        CHECK(probe.find("hevc") != std::string::npos);
        CHECK(probe.find("hvc1") != std::string::npos);
    }
    std::filesystem::remove(file);
    ::MFShutdown();
    ::CoUninitialize();
}

TEST(Codecs_NvencWritesAv1WhereTheCardCan) {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
    const auto file = std::filesystem::temp_directory_path() / "reframe-av1-test.mp4";
    const std::string failure = EncodeWithNvenc(Codec::AV1, file);
    if (failure == "no NVIDIA card" || failure.find("cannot encode") != std::string::npos) {
        SKIP("this card has no AV1 encoder");
    } else {
        if (!failure.empty()) std::printf("    %s\n", failure.c_str());
        CHECK(failure.empty());
        CHECK(Probe(file).find("av1") != std::string::npos);
    }
    std::filesystem::remove(file);
    ::MFShutdown();
    ::CoUninitialize();
}
