#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

#include <wrl/client.h>

#include "rf/core/Log.h"
#include "rf/core/Strings.h"
#include "rf/encode/AmfEncoder.h"
#include "rf/encode/IVideoEncoder.h"
#include "rf/encode/MfEncoder.h"
#include "rf/encode/NvencEncoder.h"

using Microsoft::WRL::ComPtr;

namespace rf {
namespace {

GUID SubtypeFor(Codec c) {
    switch (c) {
        case Codec::H264: return MFVideoFormat_H264;
        case Codec::HEVC: return MFVideoFormat_HEVC;
        case Codec::AV1:  return MFVideoFormat_AV1;
    }
    return MFVideoFormat_H264;
}

void ProbeMfCodec(Codec codec, std::vector<EncoderCapability>& out) {
    MFT_REGISTER_TYPE_INFO in_info{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO out_info{MFMediaType_Video, SubtypeFor(codec)};

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                         MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in_info, &out_info,
                         &activates, &count)))
        return;

    for (UINT32 i = 0; i < count; ++i) {
        EncoderCapability cap;
        cap.backend = EncoderBackend::MediaFoundation;
        cap.codec = codec;
        cap.hardware = true;

        LPWSTR friendly = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(activates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly,
                                                       &len)) &&
            friendly) {
            cap.name = ToUtf8(friendly);
            ::CoTaskMemFree(friendly);
        }
        out.push_back(std::move(cap));
        activates[i]->Release();
    }
    ::CoTaskMemFree(activates);
}

}

const char* ToString(EncoderBackend b) {
    switch (b) {
        case EncoderBackend::Auto:            return "auto";
        case EncoderBackend::Nvenc:           return "NVENC";
        case EncoderBackend::Amf:             return "AMF";
        case EncoderBackend::MediaFoundation: return "Media Foundation";
    }
    return "?";
}

std::vector<EncoderCapability> ProbeEncoders(const D3DDevicePtr& device) {
    std::vector<EncoderCapability> caps;

    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        RF_ERROR("MFStartup failed - cannot probe encoders");
        return caps;
    }

    for (Codec codec : {Codec::H264, Codec::HEVC, Codec::AV1}) ProbeMfCodec(codec, caps);
    MFShutdown();

    if (device && device->info().vendor == GpuVendor::Nvidia && NvencEncoder::Available()) {
        caps.push_back({EncoderBackend::Nvenc, Codec::H264, "NVENC (direct, nvEncodeAPI64.dll)",
                        true, true, true, 0, 0});
    }
    if (device && device->info().vendor == GpuVendor::Amd && AmfEncoder::Available()) {
        caps.push_back(
            {EncoderBackend::Amf, Codec::H264, "AMF (direct, amfrt64.dll)", true, true, false, 0, 0});
    }

    return caps;
}

Status CreateVideoEncoder(const D3DDevicePtr& device, EncoderBackend backend,
                          VideoEncoderPtr& out) {
    if (!device) return Status::Fail("null D3D device");

    if (backend == EncoderBackend::Auto) {
        switch (device->info().vendor) {
            case GpuVendor::Nvidia:
                backend = NvencEncoder::Available() ? EncoderBackend::Nvenc
                                                    : EncoderBackend::MediaFoundation;
                break;
            case GpuVendor::Amd:
                backend = AmfEncoder::Available() ? EncoderBackend::Amf
                                                  : EncoderBackend::MediaFoundation;
                break;
            default:
                backend = EncoderBackend::MediaFoundation;
                break;
        }
    }

    switch (backend) {
        case EncoderBackend::Nvenc: out = std::make_unique<NvencEncoder>(device); break;
        case EncoderBackend::Amf:   out = std::make_unique<AmfEncoder>(device); break;
        default:                    out = std::make_unique<MfEncoder>(device); break;
    }

    RF_INFO("encoder backend: {} on {}", ToString(backend), ToString(device->info().vendor));
    return Status::Ok();
}

}
