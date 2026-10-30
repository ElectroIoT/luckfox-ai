#include "motion.h"

#include <cstdlib>

namespace aid {

bool MotionDetector::process(const uint8_t* y, int width, int height, int stride, int thresholdPct,
                             float* changedPct, int pixelDelta) {
    if (changedPct) *changedPct = 0;
    if (!y || width < 16 || height < 16) return false;
    const int gw = 80, gh = 45;
    std::vector<uint8_t> cur((size_t)gw * gh);
    for (int j = 0; j < gh; ++j) {
        const int sy = (int)(((long long)j * height) / gh + height / (2 * gh));
        const uint8_t* row = y + (size_t)sy * stride;
        for (int i = 0; i < gw; ++i) {
            const int sx = (int)(((long long)i * width) / gw + width / (2 * gw));
            cur[(size_t)j * gw + i] = row[sx];
        }
    }
    bool moving = false;
    if (prev_.size() == cur.size() && gw_ == gw && gh_ == gh) {
        int changed = 0;
        for (size_t k = 0; k < cur.size(); ++k)
            if (abs((int)cur[k] - (int)prev_[k]) > pixelDelta) ++changed;
        const float pct = 100.0f * (float)changed / (float)cur.size();
        if (changedPct) *changedPct = pct;
        moving = pct >= (float)thresholdPct;
    }
    prev_.swap(cur);
    gw_ = gw;
    gh_ = gh;
    return moving;
}

}  // namespace aid
