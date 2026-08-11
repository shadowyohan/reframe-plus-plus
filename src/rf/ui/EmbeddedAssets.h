#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace rf::assets {

struct Blob {
    const unsigned char* data = nullptr;
    std::size_t size = 0;

    [[nodiscard]] bool empty() const { return data == nullptr || size == 0; }
};

Blob Find(std::string_view name);

std::vector<std::string> List(std::string_view folder);

}
