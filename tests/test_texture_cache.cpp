#include <string>
#include <vector>

#include "rf/gpu/D3DDevice.h"
#include "rf/ui/Assets.h"
#include "test_framework.h"

using rf::D3DDevice;
using rf::D3DDevicePtr;
using rf::ui::TextureCache;

TEST(TextureCache_KeepsWhatIsStillBeingShown) {
    D3DDevicePtr device;
    if (auto s = D3DDevice::CreateForOutput(nullptr, device); !s.ok() || !device) return;

    TextureCache cache;
    cache.Init(device->device());

    const std::vector<std::uint8_t> pixel(4, 255);
    const auto upload = [&](const std::string& key) { cache.FromRgba(key, pixel.data(), 1, 1); };

    upload("newest");
    for (int i = 0; i < 191; ++i) upload("old-" + std::to_string(i));

    CHECK(cache.Contains("newest"));
    upload("one-more-old");

    CHECK(cache.Contains("newest"));
    CHECK(!cache.Contains("old-0"));
}
