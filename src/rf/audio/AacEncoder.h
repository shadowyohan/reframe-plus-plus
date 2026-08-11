#pragma once
#include <mfidl.h>
#include <mftransform.h>

#include <functional>
#include <vector>

#include <wrl/client.h>

#include "rf/core/Media.h"
#include "rf/core/Status.h"

namespace rf {

class AacEncoder {
public:
    ~AacEncoder();

    Status Open(const AudioFormat& format, std::uint32_t bitrate_bps, Ticks100ns epoch,
                const std::function<void(PacketPtr)>& on_packet);
    void Close();

    Status Feed(const float* interleaved, std::uint32_t frames, Ticks100ns timestamp);

    [[nodiscard]] IMFMediaType* output_type() const { return output_type_.Get(); }

private:
    Status Drain();

    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    Microsoft::WRL::ComPtr<IMFMediaType> output_type_;
    std::function<void(PacketPtr)> on_packet_;
    std::vector<std::int16_t> convert_;
    AudioFormat format_{};
    Ticks100ns epoch_ = 0;
    bool open_ = false;
};

}
