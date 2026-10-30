// A read-only view of one camera frame (NV12), valid only inside the callback that receives it.
#pragma once

#include <cstdint>

namespace aid {

struct FrameView {
    const uint8_t* y = nullptr;   // luma plane; the interleaved UV plane follows at y + stride * vstride
    int width = 0;
    int height = 0;
    int stride = 0;               // bytes per row (virtual width)
    int vstride = 0;              // rows in the luma plane (virtual height)
    uint64_t ptsUs = 0;           // capture timestamp, microseconds (CLOCK_MONOTONIC)
};

}  // namespace aid
