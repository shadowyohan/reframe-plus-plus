#pragma once
#include <windows.h>

#include <cstdint>

namespace rf::hook::gl {

void DrawOverlay(HDC hdc, std::uint32_t shared_handle, std::uint32_t serial, unsigned width,
                 unsigned height);

void Release();

}
