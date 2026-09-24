#include "rf/mux/TrackNames.h"

#include <io.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <format>
#include <memory>

namespace rf {
namespace {

using FourCc = std::array<char, 4>;

constexpr FourCc kMoov{'m', 'o', 'o', 'v'};
constexpr FourCc kTrak{'t', 'r', 'a', 'k'};
constexpr FourCc kMdia{'m', 'd', 'i', 'a'};
constexpr FourCc kHdlr{'h', 'd', 'l', 'r'};
constexpr FourCc kUdta{'u', 'd', 't', 'a'};
constexpr FourCc kName{'n', 'a', 'm', 'e'};
constexpr FourCc kSound{'s', 'o', 'u', 'n'};
constexpr FourCc kFullMix{'r', 'f', 'm', 'x'};

constexpr std::size_t kHdlrFixedBytes = 24;
constexpr std::size_t kHdlrTypeOffset = 8;

struct Box {
    FourCc type{};
    bool container = false;
    std::vector<std::uint8_t> body;
    std::vector<Box> children;
};

std::uint32_t ReadU32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) |
           std::uint32_t{p[3]};
}

std::uint64_t ReadU64(const std::uint8_t* p) {
    return (std::uint64_t{ReadU32(p)} << 32) | ReadU32(p + 4);
}

void AppendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

bool IsContainer(const FourCc& type) {
    return type == kMoov || type == kTrak || type == kMdia || type == kUdta;
}

bool ParseBoxes(const std::uint8_t* data, std::size_t size, std::vector<Box>& out) {
    std::size_t pos = 0;
    while (pos + 8 <= size) {
        std::uint64_t length = ReadU32(data + pos);
        std::size_t header = 8;
        if (length == 1) {
            if (pos + 16 > size) return false;
            length = ReadU64(data + pos + 8);
            header = 16;
        } else if (length == 0) {
            length = size - pos;
        }
        if (length < header || pos + length > size) return false;

        Box box;
        std::memcpy(box.type.data(), data + pos + 4, 4);
        box.container = IsContainer(box.type);
        const std::uint8_t* body = data + pos + header;
        const std::size_t body_size = static_cast<std::size_t>(length) - header;
        if (box.container) {
            if (!ParseBoxes(body, body_size, box.children)) return false;
        } else {
            box.body.assign(body, body + body_size);
        }
        out.push_back(std::move(box));
        pos += static_cast<std::size_t>(length);
    }
    return pos == size;
}

void Serialize(const Box& box, std::vector<std::uint8_t>& out) {
    std::vector<std::uint8_t> payload;
    if (box.container) {
        for (const Box& child : box.children) Serialize(child, payload);
    } else {
        payload = box.body;
    }
    AppendU32(out, static_cast<std::uint32_t>(payload.size() + 8));
    out.insert(out.end(), box.type.begin(), box.type.end());
    out.insert(out.end(), payload.begin(), payload.end());
}

Box* Child(Box& parent, const FourCc& type) {
    const auto it = std::find_if(parent.children.begin(), parent.children.end(),
                                 [&](const Box& b) { return b.type == type; });
    return it == parent.children.end() ? nullptr : &*it;
}

Box& UserData(Box& trak) {
    if (Box* udta = Child(trak, kUdta)) return *udta;
    Box fresh;
    fresh.type = kUdta;
    fresh.container = true;
    trak.children.push_back(std::move(fresh));
    return trak.children.back();
}

void PutUserBox(Box& trak, const FourCc& type, const std::string& body) {
    Box& udta = UserData(trak);
    std::erase_if(udta.children, [&](const Box& b) { return b.type == type; });
    Box entry;
    entry.type = type;
    entry.body.assign(body.begin(), body.end());
    udta.children.push_back(std::move(entry));
}

void NameTrack(Box& trak, Box& hdlr, const std::string& name) {
    hdlr.body.resize(kHdlrFixedBytes);
    hdlr.body.insert(hdlr.body.end(), name.begin(), name.end());
    hdlr.body.push_back(0);
    PutUserBox(trak, kName, name);
}

bool IsSoundTrack(Box& trak) {
    if (trak.type != kTrak) return false;
    Box* mdia = Child(trak, kMdia);
    Box* hdlr = mdia ? Child(*mdia, kHdlr) : nullptr;
    return hdlr && hdlr->body.size() >= kHdlrFixedBytes &&
           std::memcmp(hdlr->body.data() + kHdlrTypeOffset, kSound.data(), 4) == 0;
}

struct TopLevelBox {
    FourCc type{};
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

bool ScanTopLevel(std::FILE* file, std::uint64_t file_size, std::vector<TopLevelBox>& out) {
    std::uint64_t pos = 0;
    while (pos + 8 <= file_size) {
        std::uint8_t header[16]{};
        if (_fseeki64(file, static_cast<std::int64_t>(pos), SEEK_SET) != 0 ||
            std::fread(header, 1, 16, file) < 8)
            return false;
        std::uint64_t length = ReadU32(header);
        if (length == 1) length = ReadU64(header + 8);
        else if (length == 0) length = file_size - pos;
        if (length < 8 || pos + length > file_size) return false;

        TopLevelBox box;
        std::memcpy(box.type.data(), header + 4, 4);
        box.offset = pos;
        box.size = length;
        out.push_back(box);
        pos += length;
    }
    return pos == file_size;
}

struct MovieHeader {
    std::uint64_t offset = 0;
    Box moov;
};

Status ReadMovieHeader(std::FILE* raw, const std::filesystem::path& file, MovieHeader& out) {
    std::error_code ec;
    const std::uint64_t file_size = std::filesystem::file_size(file, ec);
    std::vector<TopLevelBox> top;
    if (ec || !ScanTopLevel(raw, file_size, top) || top.empty())
        return Status::Fail("the recording is not a well-formed MP4");
    if (top.back().type != kMoov)
        return Status::Fail("the movie header is not at the end of the file");

    const TopLevelBox& moov_at = top.back();
    std::vector<std::uint8_t> moov_bytes(static_cast<std::size_t>(moov_at.size));
    if (_fseeki64(raw, static_cast<std::int64_t>(moov_at.offset), SEEK_SET) != 0 ||
        std::fread(moov_bytes.data(), 1, moov_bytes.size(), raw) != moov_bytes.size())
        return Status::Fail("cannot read the movie header");

    std::vector<Box> parsed;
    if (!ParseBoxes(moov_bytes.data(), moov_bytes.size(), parsed) || parsed.size() != 1)
        return Status::Fail("cannot parse the movie header");
    out.offset = moov_at.offset;
    out.moov = std::move(parsed.front());
    return Status::Ok();
}

}

Status WriteAudioTrackNames(const std::filesystem::path& file,
                            const std::vector<std::string>& audio_track_names,
                            bool first_track_is_full_mix) {
    std::FILE* raw = _wfopen(file.c_str(), L"r+b");
    if (!raw) return Status::Fail("cannot open the recording to name its tracks");
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> handle(raw, &std::fclose);

    MovieHeader header;
    RF_TRY(ReadMovieHeader(raw, file, header));

    std::size_t audio_index = 0;
    for (Box& trak : header.moov.children) {
        if (!IsSoundTrack(trak)) continue;
        Box& hdlr = *Child(*Child(trak, kMdia), kHdlr);
        if (audio_index < audio_track_names.size() && !audio_track_names[audio_index].empty())
            NameTrack(trak, hdlr, audio_track_names[audio_index]);
        if (audio_index == 0 && first_track_is_full_mix) PutUserBox(trak, kFullMix, {});
        ++audio_index;
    }

    std::vector<std::uint8_t> rewritten;
    Serialize(header.moov, rewritten);
    if (_fseeki64(raw, static_cast<std::int64_t>(header.offset), SEEK_SET) != 0 ||
        std::fwrite(rewritten.data(), 1, rewritten.size(), raw) != rewritten.size())
        return Status::Fail("cannot write the movie header");
    std::fflush(raw);
    if (_chsize_s(_fileno(raw), static_cast<long long>(header.offset + rewritten.size())) != 0)
        return Status::Fail("cannot trim the recording after naming its tracks");
    return Status::Ok();
}

bool FirstAudioTrackIsFullMix(const std::filesystem::path& file) {
    std::FILE* raw = _wfopen(file.c_str(), L"rb");
    if (!raw) return false;
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> handle(raw, &std::fclose);

    MovieHeader header;
    if (!ReadMovieHeader(raw, file, header).ok()) return false;
    for (Box& trak : header.moov.children) {
        if (!IsSoundTrack(trak)) continue;
        Box* udta = Child(trak, kUdta);
        return udta && Child(*udta, kFullMix);
    }
    return false;
}

}
