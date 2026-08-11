#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "rf/gpu/GpuInfo.h"

namespace rf {

struct Quirk {
    enum class Id {

        DuplRecreateOnModeChange,

        DuplIgnoreZeroPresentTime,

        WgcBorderUnsupported,

        EncoderLimitAsyncDepth,

        DisableBFrames,

        DisableSplitEncode,

        ForceEncoderOnCaptureAdapter,
    };

    Id id;
    std::string reason;
};

struct QuirkSet {
    std::vector<Quirk> quirks;

    [[nodiscard]] bool has(Quirk::Id id) const;
    [[nodiscard]] std::string describe() const;
};

QuirkSet DetectQuirks(const AdapterInfo& adapter);

}
