// Simple motion detection on a tiny grid of luma samples (cheap on a single A7 core).
#pragma once

#include <cstdint>
#include <vector>

namespace aid {

class MotionDetector {
public:
    // Samples the luma plane on a grid of about 80x45 points. Returns true when at least
    // thresholdPct percent of the samples changed by more than pixelDelta since the last call.
    // The first call only stores the reference and returns false. Also writes the measured
    // change (percent of samples) to *changedPct if not null.
    bool process(const uint8_t* y, int width, int height, int stride, int thresholdPct,
                 float* changedPct = nullptr, int pixelDelta = 25);
    void reset() { prev_.clear(); }

private:
    std::vector<uint8_t> prev_;
    int gw_ = 0, gh_ = 0;
};

}  // namespace aid
