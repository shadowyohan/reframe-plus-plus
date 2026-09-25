#include "rf/integrations/Protocol.h"

#include <algorithm>
#include <charconv>

namespace rf::integrations {
namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

bool NeedsQuotes(std::string_view value) {
    return value.empty() || std::any_of(value.begin(), value.end(), [](char c) {
               return IsSpace(c) || c == '"' || c == '\\' || c == '=';
           });
}

bool ReadQuoted(std::string_view line, std::size_t& at, std::string& out) {
    for (++at; at < line.size(); ++at) {
        const char c = line[at];
        if (c == '"') {
            ++at;
            return true;
        }
        if (c == '\\' && at + 1 < line.size() && (line[at + 1] == '"' || line[at + 1] == '\\'))
            ++at;
        out += line[at];
    }
    return false;
}

bool ReadBare(std::string_view line, std::size_t& at, std::string& out) {
    while (at < line.size() && !IsSpace(line[at]) && line[at] != '=') {
        if (line[at] == '"') return false;
        out += line[at++];
    }
    return true;
}

bool ReadValue(std::string_view line, std::size_t& at, std::string& out) {
    if (at < line.size() && line[at] == '"') return ReadQuoted(line, at, out);
    return ReadBare(line, at, out);
}

}

std::optional<std::string_view> Command::Get(std::string_view key) const {
    for (const auto& [k, v] : fields)
        if (k == key) return v;
    return std::nullopt;
}

std::optional<std::uint32_t> Command::GetNumber(std::string_view key) const {
    const auto text = Get(key);
    if (!text) return std::nullopt;
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), value);
    if (error != std::errc{} || end != text->data() + text->size()) return std::nullopt;
    return value;
}

std::optional<Command> ParseCommand(std::string_view line) {
    Command command;
    std::size_t at = 0;
    while (true) {
        while (at < line.size() && IsSpace(line[at])) ++at;
        if (at == line.size()) break;

        std::string word;
        const bool quoted = line[at] == '"';
        if (!ReadValue(line, at, word)) return std::nullopt;

        if (!quoted && at < line.size() && line[at] == '=') {
            ++at;
            std::string value;
            if (!ReadValue(line, at, value)) return std::nullopt;
            if (word.empty()) return std::nullopt;
            command.fields.emplace_back(std::move(word), std::move(value));
        } else if (command.name.empty()) {
            if (quoted) return std::nullopt;
            command.name = std::move(word);
        } else if (command.arg.empty() && command.fields.empty()) {
            command.arg = std::move(word);
        } else {
            return std::nullopt;
        }

        if (at < line.size() && !IsSpace(line[at])) return std::nullopt;
    }
    if (command.name.empty()) return std::nullopt;
    return command;
}

std::string Quote(std::string_view value) {
    if (!NeedsQuotes(value)) return std::string(value);
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

bool IsValidTag(std::string_view tag) {
    return !tag.empty() && tag.size() <= 32 && std::all_of(tag.begin(), tag.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_';
    });
}

std::string JoinTags(const std::vector<std::string>& tags) {
    std::string joined;
    for (const std::string& tag : tags) {
        if (!joined.empty()) joined += '+';
        joined += tag;
    }
    return joined;
}

}
