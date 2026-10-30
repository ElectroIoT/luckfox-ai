#include "preprocess.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace aid {

Letterbox computeLetterbox(int srcW, int srcH, int dstW, int dstH) {
    Letterbox lb;
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return lb;
    const float sx = (float)dstW / (float)srcW, sy = (float)dstH / (float)srcH;
    lb.scale = std::min(sx, sy);
    lb.newW = std::max(1, std::min(dstW, (int)((float)srcW * lb.scale + 0.5f)));
    lb.newH = std::max(1, std::min(dstH, (int)((float)srcH * lb.scale + 0.5f)));
    lb.padX = (dstW - lb.newW) / 2;
    lb.padY = (dstH - lb.newH) / 2;
    return lb;
}

static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

Letterbox nv12ToLetterboxRgb(const FrameView& f, uint8_t* dst, int dstW, int dstH, uint8_t pad) {
    const Letterbox lb = computeLetterbox(f.width, f.height, dstW, dstH);
    memset(dst, pad, (size_t)dstW * dstH * 3);
    if (!f.y || lb.newW <= 0) return lb;

    const uint8_t* uvPlane = f.y + (size_t)f.stride * f.vstride;
    std::vector<int> mapX((size_t)lb.newW);
    for (int ox = 0; ox < lb.newW; ++ox)
        mapX[ox] = std::min(f.width - 1, (int)(((long long)ox * f.width) / lb.newW));

    for (int oy = 0; oy < lb.newH; ++oy) {
        const int sy = std::min(f.height - 1, (int)(((long long)oy * f.height) / lb.newH));
        const uint8_t* yRow = f.y + (size_t)sy * f.stride;
        const uint8_t* uvRow = uvPlane + (size_t)(sy >> 1) * f.stride;
        uint8_t* out = dst + ((size_t)(oy + lb.padY) * dstW + lb.padX) * 3;
        for (int ox = 0; ox < lb.newW; ++ox) {
            const int sx = mapX[ox];
            const int c = (int)yRow[sx] - 16;
            const int d = (int)uvRow[(sx & ~1)] - 128;
            const int e = (int)uvRow[(sx & ~1) + 1] - 128;
            const int cc = 298 * c + 128;
            out[0] = clamp8((cc + 409 * e) >> 8);
            out[1] = clamp8((cc - 100 * d - 208 * e) >> 8);
            out[2] = clamp8((cc + 516 * d) >> 8);
            out += 3;
        }
    }
    return lb;
}

void boxToNormalized(const Letterbox& lb, int left, int top, int right, int bottom, float* x1,
                     float* y1, float* x2, float* y2) {
    auto norm = [](float v, float lo, float len) {
        if (len <= 0) return 0.0f;
        float r = (v - lo) / len;
        return r < 0 ? 0.0f : (r > 1 ? 1.0f : r);
    };
    *x1 = norm((float)left, (float)lb.padX, (float)lb.newW);
    *x2 = norm((float)right, (float)lb.padX, (float)lb.newW);
    *y1 = norm((float)top, (float)lb.padY, (float)lb.newH);
    *y2 = norm((float)bottom, (float)lb.padY, (float)lb.newH);
}

}  // namespace aid
