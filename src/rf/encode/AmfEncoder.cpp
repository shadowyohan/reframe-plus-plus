#include "rf/encode/AmfEncoder.h"

#include <windows.h>

#include "rf/core/Log.h"

namespace rf {

AmfEncoder::AmfEncoder(D3DDevicePtr device) : device_(std::move(device)) {}
AmfEncoder::~AmfEncoder() { Close(); }

bool AmfEncoder::Available() {
    HMODULE dll = ::LoadLibraryW(L"amfrt64.dll");
    if (!dll) return false;
    const bool has_entry = ::GetProcAddress(dll, "AMFInit") != nullptr;
    ::FreeLibrary(dll);
    return has_entry;
}

Status AmfEncoder::Open(const EncoderConfig& config, const PacketCallback& on_packet) {
    config_ = config;
    on_packet_ = on_packet;

#ifndef RF_HAS_AMF
    return Status::Fail("AMF backend not built - configure with -DRF_AMF_SDK_DIR=<AMF SDK>");
#else

    return Status::Fail("AMF backend is scaffolded but not implemented yet");
#endif
}

void AmfEncoder::Close() {}

Status AmfEncoder::Submit(const CapturedFrame&) {
    return Status::Fail("AMF backend not implemented");
}

Status AmfEncoder::RequestKeyframe() { return Status::Fail("AMF backend not implemented"); }
Status AmfEncoder::Flush() { return Status::Ok(); }

}
