#include "rf/core/Status.h"

#include <format>

namespace rf {

std::string Status::str() const {
    if (ok()) return "ok";
    return std::format("{} (0x{:08X})", msg_, static_cast<unsigned>(hr_));
}

}
