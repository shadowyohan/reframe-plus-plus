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
constexpr FourCc kMinf{'m', 'i', 'n', 'f'};
constexpr FourCc kStbl{'s', 't', 'b', 'l'};
constexpr FourCc kMvhd{'m', 'v', 'h', 'd'};
constexpr FourCc kTkhd{'t', 'k', 'h', 'd'};
constexpr FourCc kMdhd{'m', 'd', 'h', 'd'};
constexpr FourCc kStts{'s', 't', 't', 's'};
constexpr FourCc kName{'n', 'a', 'm', 'e'};
constexpr FourCc kSound{'s', 'o', 'u', 'n'};
constexpr FourCc kFullMix{'r', 'f', 'm', 'x'};
constexpr FourCc kMarkers{'r', 'f', 'm', 'k'};
constexpr FourCc kNeroChapters{'c', 'h', 'p', 'l'};
constexpr FourCc kUuid{'u', 'u', 'i', 'd'};
constexpr FourCc kFree{'f', 'r', 'e', 'e'};
constexpr std::array<std::uint8_t, 16> kXmpUuid{0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9, 0x42, 0xE8,
                                                0x9C, 0x71, 0x99, 0x94, 0x91, 0xE3, 0xAF, 0xAC};
constexpr std::size_t kLongestMarkerTag = 255;

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

void WriteU32At(std::vector<std::uint8_t>& body, std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) body[at + i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
}

void WriteU64At(std::vector<std::uint8_t>& body, std::size_t at, std::uint64_t value) {
    WriteU32At(body, at, static_cast<std::uint32_t>(value >> 32));
    WriteU32At(body, at + 4, static_cast<std::uint32_t>(value));
}

struct TimedHeader {
    std::size_t timescale_at = 0;
    std::size_t duration_at = 0;
    bool wide = false;
};

TimedHeader HeaderLayout(const std::vector<std::uint8_t>& body, bool track_header) {
    const bool wide = !body.empty() && body[0] == 1;
    if (track_header) return {0, wide ? 28u : 20u, wide};
    return {wide ? 20u : 12u, wide ? 24u : 16u, wide};
}

std::uint64_t ReadDuration(const std::vector<std::uint8_t>& body, const TimedHeader& at) {
    if (body.size() < at.duration_at + (at.wide ? 8 : 4)) return 0;
    return at.wide ? ReadU64(body.data() + at.duration_at) : ReadU32(body.data() + at.duration_at);
}

void WriteDuration(std::vector<std::uint8_t>& body, const TimedHeader& at, std::uint64_t value) {
    if (body.size() < at.duration_at + (at.wide ? 8 : 4)) return;
    if (at.wide)
        WriteU64At(body, at.duration_at, value);
    else
        WriteU32At(body, at.duration_at, static_cast<std::uint32_t>(std::min<std::uint64_t>(value, 0xFFFFFFFFu)));
}

std::uint64_t SampleTableLength(const Box& stts) {
    if (stts.body.size() < 8) return 0;
    const std::uint32_t entries = ReadU32(stts.body.data() + 4);
    std::uint64_t total = 0;
    for (std::uint32_t i = 0; i < entries && 8 + i * 8 + 8 <= stts.body.size(); ++i)
        total += std::uint64_t{ReadU32(stts.body.data() + 8 + i * 8)} * ReadU32(stts.body.data() + 12 + i * 8);
    return total;
}

void AppendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void AppendU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    AppendU32(out, static_cast<std::uint32_t>(value >> 32));
    AppendU32(out, static_cast<std::uint32_t>(value));
}

bool IsContainer(const FourCc& type) {
    return type == kMoov || type == kTrak || type == kMdia || type == kUdta || type == kMinf ||
           type == kStbl;
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
    std::vector<Box> trailing;
};

bool CanFollowTheMovie(const FourCc& type) { return type == kUuid || type == kFree; }

Status ReadMovieHeader(std::FILE* raw, const std::filesystem::path& file, MovieHeader& out) {
    std::error_code ec;
    const std::uint64_t file_size = std::filesystem::file_size(file, ec);
    std::vector<TopLevelBox> top;
    if (ec || !ScanTopLevel(raw, file_size, top) || top.empty())
        return Status::Fail("the recording is not a well-formed MP4");
    const auto moov_it = std::find_if(top.rbegin(), top.rend(),
                                      [](const TopLevelBox& b) { return b.type == kMoov; });
    if (moov_it == top.rend() ||
        !std::all_of(top.rbegin(), moov_it, [](const TopLevelBox& b) { return CanFollowTheMovie(b.type); }))
        return Status::Fail("the movie header is not at the end of the file");

    const TopLevelBox& moov_at = *moov_it;
    const std::uint64_t tail_offset = moov_at.offset + moov_at.size;
    std::vector<std::uint8_t> tail_bytes(static_cast<std::size_t>(file_size - tail_offset));
    if (!tail_bytes.empty() &&
        (_fseeki64(raw, static_cast<std::int64_t>(tail_offset), SEEK_SET) != 0 ||
         std::fread(tail_bytes.data(), 1, tail_bytes.size(), raw) != tail_bytes.size() ||
         !ParseBoxes(tail_bytes.data(), tail_bytes.size(), out.trailing)))
        return Status::Fail("cannot read what follows the movie header");

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

Status WriteMovieHeader(std::FILE* raw, const MovieHeader& header) {
    std::vector<std::uint8_t> rewritten;
    Serialize(header.moov, rewritten);
    for (const Box& box : header.trailing) Serialize(box, rewritten);
    if (_fseeki64(raw, static_cast<std::int64_t>(header.offset), SEEK_SET) != 0 ||
        std::fwrite(rewritten.data(), 1, rewritten.size(), raw) != rewritten.size())
        return Status::Fail("cannot write the movie header");
    std::fflush(raw);
    if (_chsize_s(_fileno(raw), static_cast<long long>(header.offset + rewritten.size())) != 0)
        return Status::Fail("cannot trim the file after rewriting its movie header");
    return Status::Ok();
}

bool IsXmpBox(const Box& box) {
    return box.type == kUuid && box.body.size() >= kXmpUuid.size() &&
           std::equal(kXmpUuid.begin(), kXmpUuid.end(), box.body.begin());
}

std::string XmlEscaped(const std::string& text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::vector<std::uint8_t> MarkerBody(const std::vector<ClipMarker>& markers) {
    std::vector<std::uint8_t> body;
    AppendU32(body, static_cast<std::uint32_t>(markers.size()));
    for (const ClipMarker& marker : markers) {
        const std::size_t length = std::min(marker.tag.size(), kLongestMarkerTag);
        AppendU32(body, marker.ms);
        body.push_back(static_cast<std::uint8_t>(length));
        body.insert(body.end(), marker.tag.begin(), marker.tag.begin() + static_cast<std::ptrdiff_t>(length));
    }
    return body;
}

std::vector<std::uint8_t> NeroChapterBody(const std::vector<ClipMarker>& markers) {
    std::vector<std::uint8_t> body{1, 0, 0, 0, 0, 0, 0, 0};
    const std::size_t count = std::min<std::size_t>(markers.size(), 255);
    body.push_back(static_cast<std::uint8_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
        const std::string title = markers[i].tag.empty() ? std::string("moment") : markers[i].tag;
        const std::size_t length = std::min(title.size(), kLongestMarkerTag);
        AppendU64(body, static_cast<std::uint64_t>(markers[i].ms) * 10'000);
        body.push_back(static_cast<std::uint8_t>(length));
        body.insert(body.end(), title.begin(), title.begin() + static_cast<std::ptrdiff_t>(length));
    }
    return body;
}

}

std::string MarkersAsXmp(const std::vector<ClipMarker>& markers) {
    std::string xmp =
        "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
        "<rdf:Description rdf:about=\"\" xmlns:xmpDM=\"http://ns.adobe.com/xmp/1.0/DynamicMedia/\">"
        "<xmpDM:Tracks><rdf:Bag><rdf:li rdf:parseType=\"Resource\">"
        "<xmpDM:trackName>Comment</xmpDM:trackName><xmpDM:trackType>Comment</xmpDM:trackType>"
        "<xmpDM:frameRate>f1000</xmpDM:frameRate><xmpDM:markers><rdf:Seq>";
    for (const ClipMarker& marker : markers)
        xmp += std::format("<rdf:li rdf:parseType=\"Resource\"><xmpDM:startTime>{}</xmpDM:startTime>"
                           "<xmpDM:duration>0</xmpDM:duration><xmpDM:name>{}</xmpDM:name></rdf:li>",
                           marker.ms, XmlEscaped(marker.tag));
    xmp += "</rdf:Seq></xmpDM:markers></rdf:li></rdf:Bag></xmpDM:Tracks></rdf:Description>"
           "</rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>";
    return xmp;
}

Status WriteClipMarkers(const std::filesystem::path& file, const std::vector<ClipMarker>& markers) {
    std::FILE* raw = _wfopen(file.c_str(), L"r+b");
    if (!raw) return Status::Fail("cannot open the clip to mark its moments");
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> handle(raw, &std::fclose);

    MovieHeader header;
    RF_TRY(ReadMovieHeader(raw, file, header));

    Box& udta = UserData(header.moov);
    std::erase_if(udta.children,
                  [](const Box& b) { return b.type == kMarkers || b.type == kNeroChapters; });
    std::erase_if(header.trailing, IsXmpBox);

    if (!markers.empty()) {
        Box own;
        own.type = kMarkers;
        own.body = MarkerBody(markers);
        udta.children.push_back(std::move(own));

        Box chapters;
        chapters.type = kNeroChapters;
        chapters.body = NeroChapterBody(markers);
        udta.children.push_back(std::move(chapters));

        Box xmp;
        xmp.type = kUuid;
        xmp.body.assign(kXmpUuid.begin(), kXmpUuid.end());
        const std::string packet = MarkersAsXmp(markers);
        xmp.body.insert(xmp.body.end(), packet.begin(), packet.end());
        header.trailing.push_back(std::move(xmp));
    }
    return WriteMovieHeader(raw, header);
}

std::vector<ClipMarker> ReadClipMarkers(const std::filesystem::path& file) {
    std::vector<ClipMarker> markers;
    std::FILE* raw = _wfopen(file.c_str(), L"rb");
    if (!raw) return markers;
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> handle(raw, &std::fclose);

    MovieHeader header;
    if (!ReadMovieHeader(raw, file, header).ok()) return markers;
    Box* udta = Child(header.moov, kUdta);
    Box* own = udta ? Child(*udta, kMarkers) : nullptr;
    if (!own || own->body.size() < 4) return markers;

    const std::vector<std::uint8_t>& body = own->body;
    const std::uint32_t count = ReadU32(body.data());
    std::size_t at = 4;
    for (std::uint32_t i = 0; i < count && at + 5 <= body.size(); ++i) {
        ClipMarker marker;
        marker.ms = ReadU32(body.data() + at);
        const std::size_t length = body[at + 4];
        at += 5;
        if (at + length > body.size()) break;
        marker.tag.assign(reinterpret_cast<const char*>(body.data() + at), length);
        at += length;
        markers.push_back(std::move(marker));
    }
    return markers;
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
    return WriteMovieHeader(raw, header);
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

std::vector<std::string> ReadAudioTrackNames(const std::filesystem::path& file) {
    std::vector<std::string> names;
    std::FILE* raw = _wfopen(file.c_str(), L"rb");
    if (!raw) return names;
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> handle(raw, &std::fclose);

    MovieHeader header;
    if (!ReadMovieHeader(raw, file, header).ok()) return names;
    for (Box& trak : header.moov.children) {
        if (!IsSoundTrack(trak)) continue;
        Box* udta = Child(trak, kUdta);
        Box* name = udta ? Child(*udta, kName) : nullptr;
        names.emplace_back(name ? std::string(name->body.begin(), name->body.end()) : std::string{});
    }
    return names;
}

Status RepairDurations(const std::filesystem::path& file) {
    std::FILE* raw = _wfopen(file.c_str(), L"r+b");
    if (!raw) return Status::Fail("cannot open the clip to fix its length");
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> handle(raw, &std::fclose);

    MovieHeader header;
    RF_TRY(ReadMovieHeader(raw, file, header));
    Box* mvhd = Child(header.moov, kMvhd);
    if (!mvhd) return Status::Fail("the movie has no header");
    const TimedHeader movie_at = HeaderLayout(mvhd->body, false);
    const std::uint32_t movie_scale = ReadU32(mvhd->body.data() + movie_at.timescale_at);
    if (movie_scale == 0) return Status::Fail("the movie has no time scale");

    std::uint64_t longest = 0;
    for (Box& trak : header.moov.children) {
        if (trak.type != kTrak) continue;
        Box* mdia = Child(trak, kMdia);
        Box* mdhd = mdia ? Child(*mdia, kMdhd) : nullptr;
        Box* minf = mdia ? Child(*mdia, kMinf) : nullptr;
        Box* stbl = minf ? Child(*minf, kStbl) : nullptr;
        Box* stts = stbl ? Child(*stbl, kStts) : nullptr;
        Box* tkhd = Child(trak, kTkhd);
        if (!mdhd || !stts || !tkhd) continue;

        const TimedHeader media_at = HeaderLayout(mdhd->body, false);
        const std::uint32_t media_scale = ReadU32(mdhd->body.data() + media_at.timescale_at);
        if (media_scale == 0) continue;
        const std::uint64_t media_length = SampleTableLength(*stts);
        WriteDuration(mdhd->body, media_at, media_length);
        const std::uint64_t track_length = media_length * movie_scale / media_scale;
        WriteDuration(tkhd->body, HeaderLayout(tkhd->body, true), track_length);
        longest = std::max(longest, track_length);
    }
    if (longest > 0) WriteDuration(mvhd->body, movie_at, longest);
    return WriteMovieHeader(raw, header);
}

}
