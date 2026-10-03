#include "rf/mux/TrackNames.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "test_framework.h"

using namespace rf;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes MakeBox(const char* type, const Bytes& payload) {
    const auto size = static_cast<std::uint32_t>(payload.size() + 8);
    Bytes box{static_cast<std::uint8_t>(size >> 24), static_cast<std::uint8_t>(size >> 16),
              static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size)};
    box.insert(box.end(), type, type + 4);
    box.insert(box.end(), payload.begin(), payload.end());
    return box;
}

Bytes Concat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
}

Bytes Handler(const char* kind, const char* name) {
    Bytes body(8, 0);
    body.insert(body.end(), kind, kind + 4);
    body.resize(24, 0);
    body.insert(body.end(), name, name + std::char_traits<char>::length(name));
    body.push_back(0);
    return MakeBox("hdlr", body);
}

Bytes Track(const char* kind) {
    return MakeBox("trak", MakeBox("mdia", Handler(kind, "Handler")));
}

Bytes ReadAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), {});
}

bool Contains(const Bytes& haystack, const std::string& needle) {
    const Bytes wanted(needle.begin(), needle.end());
    return std::search(haystack.begin(), haystack.end(), wanted.begin(), wanted.end()) !=
           haystack.end();
}

}

TEST(TrackNames_AudioTracksGetTheirNamesAndMediaStaysIntact) {
    const Bytes media(4096, 0xAB);
    const Bytes original = Concat({MakeBox("ftyp", Bytes(16, 1)), MakeBox("mdat", media),
                                   MakeBox("moov", Concat({Track("vide"), Track("soun"),
                                                           Track("soun")}))});
    const auto path = std::filesystem::temp_directory_path() / "reframe-tests" / "names.mp4";
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(original.data()),
                  static_cast<std::streamsize>(original.size()));
    }

    CHECK(WriteAudioTrackNames(path, {"Звук системы", "Discord"}).ok());

    const Bytes result = ReadAll(path);
    CHECK(Contains(result, "Discord"));
    CHECK(Contains(result, "Звук системы"));
    CHECK(Contains(result, "name"));
    CHECK(std::equal(original.begin(), original.begin() + 24 + 8 + 4096, result.begin()));

    const std::uint32_t moov_size = (std::uint32_t{result[4128]} << 24) |
                                    (std::uint32_t{result[4129]} << 16) |
                                    (std::uint32_t{result[4130]} << 8) | result[4131];
    CHECK_EQ(static_cast<std::size_t>(4128 + moov_size), result.size());
}

TEST(TrackNames_RejectsFilesWithTheHeaderUpFront) {
    const Bytes original = Concat({MakeBox("moov", Track("soun")), MakeBox("mdat", Bytes(64, 0))});
    const auto path = std::filesystem::temp_directory_path() / "reframe-tests" / "front.mp4";
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(original.data()),
                  static_cast<std::streamsize>(original.size()));
    }
    CHECK(!WriteAudioTrackNames(path, {"Discord"}).ok());
    CHECK(ReadAll(path) == original);
}

TEST(TrackNames_FullMixMarkerIsReadBack) {
    const Bytes original = Concat({MakeBox("ftyp", Bytes(16, 1)), MakeBox("mdat", Bytes(64, 0)),
                                   MakeBox("moov", Concat({Track("vide"), Track("soun"),
                                                           Track("soun")}))});
    const auto path = std::filesystem::temp_directory_path() / "reframe-tests" / "mix.mp4";
    std::filesystem::create_directories(path.parent_path());
    auto write_original = [&] {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(original.data()),
                  static_cast<std::streamsize>(original.size()));
    };

    write_original();
    CHECK(!FirstAudioTrackIsFullMix(path));
    CHECK(WriteAudioTrackNames(path, {"Все звуки", "Микрофон"}, false).ok());
    CHECK(!FirstAudioTrackIsFullMix(path));

    write_original();
    CHECK(WriteAudioTrackNames(path, {"Все звуки", "Микрофон"}, true).ok());
    CHECK(FirstAudioTrackIsFullMix(path));
}

TEST(ClipMarkers_RoundTripAndSurviveRenamingTheTracks) {
    const Bytes original = Concat({MakeBox("ftyp", Bytes(16, 1)), MakeBox("mdat", Bytes(512, 0xCD)),
                                   MakeBox("moov", Track("soun"))});
    const auto path = std::filesystem::temp_directory_path() / "rf_markers_test.mp4";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(original.data()),
                  static_cast<std::streamsize>(original.size()));
    }

    const std::vector<ClipMarker> markers{{1500, "kill"}, {4200, "ace"}};
    CHECK(WriteClipMarkers(path, markers).ok());
    CHECK(ReadClipMarkers(path) == markers);

    CHECK(WriteAudioTrackNames(path, {"Game"}).ok());
    CHECK(ReadClipMarkers(path) == markers);

    const Bytes written = ReadAll(path);
    CHECK(Contains(written, "chpl"));
    CHECK(Contains(written, "<xmpDM:name>ace</xmpDM:name>"));
    CHECK(Contains(written, std::string(512, '\xCD')));

    CHECK(WriteClipMarkers(path, markers).ok());
    const Bytes rewritten = ReadAll(path);
    CHECK_EQ(rewritten.size(), written.size());
    std::filesystem::remove(path);
}

TEST(ClipMarkers_ShowUpAsChaptersInOtherTools) {
    const auto path = std::filesystem::temp_directory_path() / "rf_markers_ffprobe.mp4";
    const std::string make = "ffmpeg -v error -y -f lavfi -i testsrc=duration=5:size=160x90:rate=10 \"" +
                             path.string() + "\" 2>nul";
    if (std::system(make.c_str()) != 0 || !std::filesystem::exists(path)) {
        SKIP("ffmpeg is not installed");
        return;
    }
    CHECK(WriteClipMarkers(path, {{1500, "kill"}, {4200, "ace"}}).ok());

    const std::string probe = "ffprobe -v error -show_chapters -of csv=p=0 \"" + path.string() + "\"";
    std::string output;
    if (std::FILE* pipe = _popen(probe.c_str(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), pipe)) output += line;
        _pclose(pipe);
    }
    CHECK(output.find("1.500000") != std::string::npos);
    CHECK(output.find("ace") != std::string::npos);
    std::filesystem::remove(path);
}
