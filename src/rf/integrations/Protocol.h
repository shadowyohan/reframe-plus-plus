#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rf::integrations {

inline constexpr std::uint32_t kProtocolVersion = 1;
inline constexpr std::size_t kMaxMessageBytes = 4096;

struct Command {
    std::string name;
    std::string arg;
    std::vector<std::pair<std::string, std::string>> fields;

    [[nodiscard]] std::optional<std::string_view> Get(std::string_view key) const;
    [[nodiscard]] std::optional<std::uint32_t> GetNumber(std::string_view key) const;
    [[nodiscard]] bool Has(std::string_view key) const { return Get(key).has_value(); }
};

[[nodiscard]] std::optional<Command> ParseCommand(std::string_view line);

[[nodiscard]] std::string Quote(std::string_view value);

[[nodiscard]] bool IsValidTag(std::string_view tag);

[[nodiscard]] std::string JoinTags(const std::vector<std::string>& tags);

}
