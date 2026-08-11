#include "rf/core/Media.h"

namespace rf {

const char* ToString(Codec c) {
    switch (c) {
        case Codec::H264: return "H.264";
        case Codec::HEVC: return "HEVC";
        case Codec::AV1:  return "AV1";
    }
    return "?";
}

const char* ToString(ColorSpace cs) {
    switch (cs) {
        case ColorSpace::Rec709:    return "Rec.709 SDR";
        case ColorSpace::Rec2020Pq: return "Rec.2020 PQ HDR";
    }
    return "?";
}

}
