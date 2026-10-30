// PC-side tests for the pure AI helpers: letterbox geometry, NV12->RGB conversion, box mapping,
// motion detection. (The NPU itself can only be tested on the board.)
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "motion.h"
#include "preprocess.h"

static int g_fail = 0, g_run = 0;
#define CHECK(cond)                                                   \
    do {                                                              \
        ++g_run;                                                      \
        if (!(cond)) {                                                \
            ++g_fail;                                                 \
            printf("FAIL line %d: %s\n", __LINE__, #cond);            \
        }                                                             \
    } while (0)

// Builds an NV12 frame filled with one colour (Y,U,V).
struct Frame {
    std::vector<uint8_t> data;
    aid::FrameView v;
    Frame(int w, int h, int stride, uint8_t Y, uint8_t U, uint8_t V) {
        data.assign((size_t)stride * h * 3 / 2, 0);
        for (int r = 0; r < h; ++r) memset(&data[(size_t)r * stride], Y, (size_t)w);
        uint8_t* uv = &data[(size_t)stride * h];
        for (int r = 0; r < h / 2; ++r)
            for (int c = 0; c < w; c += 2) { uv[(size_t)r * stride + c] = U; uv[(size_t)r * stride + c + 1] = V; }
        v.y = data.data(); v.width = w; v.height = h; v.stride = stride; v.vstride = h;
    }
};

static void testLetterbox() {
    aid::Letterbox lb = aid::computeLetterbox(1920, 1080, 640, 640);
    CHECK(lb.newW == 640 && lb.newH == 360 && lb.padX == 0 && lb.padY == 140);
    CHECK(std::fabs(lb.scale - 1.0f / 3.0f) < 1e-4);
    lb = aid::computeLetterbox(640, 640, 640, 640);
    CHECK(lb.newW == 640 && lb.newH == 640 && lb.padX == 0 && lb.padY == 0);
    lb = aid::computeLetterbox(1080, 1920, 640, 640);                 // portrait
    CHECK(lb.newH == 640 && lb.newW == 360 && lb.padX == 140 && lb.padY == 0);
    lb = aid::computeLetterbox(0, 10, 640, 640);
    CHECK(lb.newW == 0);

    // box mapping: a box covering the whole letterboxed image maps to 0..1
    lb = aid::computeLetterbox(1920, 1080, 640, 640);
    float x1, y1, x2, y2;
    aid::boxToNormalized(lb, 0, 140, 640, 500, &x1, &y1, &x2, &y2);
    CHECK(x1 == 0 && y1 == 0 && x2 == 1 && y2 == 1);
    aid::boxToNormalized(lb, 320, 320, 640, 500, &x1, &y1, &x2, &y2);   // lower right quarter-ish
    CHECK(std::fabs(x1 - 0.5f) < 1e-3 && std::fabs(y1 - 0.5f) < 1e-3 && x2 == 1 && y2 == 1);
    aid::boxToNormalized(lb, 0, 0, 640, 100, &x1, &y1, &x2, &y2);       // entirely in the padding
    CHECK(y1 == 0 && y2 == 0);
}

static void testConversion() {
    const int dw = 64, dh = 64;
    std::vector<uint8_t> out((size_t)dw * dh * 3);

    // Mid grey (Y=126, neutral chroma) -> about 128 in every channel; padding rows are 114.
    Frame grey(160, 90, 192, 126, 128, 128);          // stride 192 > width: padding bytes must be ignored
    aid::Letterbox lb = aid::nv12ToLetterboxRgb(grey.v, out.data(), dw, dh);
    CHECK(lb.newW == 64 && lb.newH == 36 && lb.padY == 14);
    const uint8_t* mid = &out[((size_t)32 * dw + 32) * 3];
    CHECK(std::abs(mid[0] - 128) <= 2 && std::abs(mid[1] - 128) <= 2 && std::abs(mid[2] - 128) <= 2);
    const uint8_t* top = &out[((size_t)2 * dw + 5) * 3];
    CHECK(top[0] == 114 && top[1] == 114 && top[2] == 114);
    const uint8_t* bottom = &out[((size_t)62 * dw + 5) * 3];
    CHECK(bottom[0] == 114);

    // Pure black / white (limited range: Y=16 -> 0, Y=235 -> 255)
    Frame black(160, 90, 160, 16, 128, 128), white(160, 90, 160, 235, 128, 128);
    aid::nv12ToLetterboxRgb(black.v, out.data(), dw, dh);
    CHECK(out[((size_t)32 * dw + 10) * 3] == 0);
    aid::nv12ToLetterboxRgb(white.v, out.data(), dw, dh);
    CHECK(out[((size_t)32 * dw + 10) * 3] == 255 && out[((size_t)32 * dw + 10) * 3 + 2] == 255);

    // Colour: strong red (Y=81,U=90,V=240) must come out red-dominant; blue (Y=41,U=240,V=110) blue-dominant.
    Frame red(160, 90, 160, 81, 90, 240), blue(160, 90, 160, 41, 240, 110);
    aid::nv12ToLetterboxRgb(red.v, out.data(), dw, dh);
    const uint8_t* r = &out[((size_t)32 * dw + 20) * 3];
    CHECK(r[0] > 200 && r[1] < 60 && r[2] < 60);
    aid::nv12ToLetterboxRgb(blue.v, out.data(), dw, dh);
    const uint8_t* b = &out[((size_t)32 * dw + 20) * 3];
    CHECK(b[2] > 200 && b[0] < 60);

    // Left half white, right half black: the left side of the output is bright, the right dark.
    Frame split(160, 90, 160, 16, 128, 128);
    for (int row = 0; row < 90; ++row)
        for (int c = 0; c < 80; ++c) split.data[(size_t)row * 160 + c] = 235;
    aid::nv12ToLetterboxRgb(split.v, out.data(), dw, dh);
    CHECK(out[((size_t)32 * dw + 8) * 3] > 240 && out[((size_t)32 * dw + 56) * 3] < 15);

    // A null frame only produces padding and must not crash.
    aid::FrameView none;
    aid::nv12ToLetterboxRgb(none, out.data(), dw, dh);
    CHECK(out[0] == 114);
}

static void testMotion() {
    aid::MotionDetector md;
    float pct = -1;
    Frame a(320, 180, 320, 100, 128, 128);
    CHECK(!md.process(a.v.y, 320, 180, 320, 3, &pct));     // first frame: reference only
    CHECK(!md.process(a.v.y, 320, 180, 320, 3, &pct) && pct == 0);   // identical frame

    // sensor noise (+-5) is below the pixel delta
    Frame noisy(320, 180, 320, 100, 128, 128);
    for (size_t i = 0; i < 320 * 180; ++i) noisy.data[i] = (uint8_t)(100 + (int)(i % 11) - 5);
    CHECK(!md.process(noisy.v.y, 320, 180, 320, 3, &pct));
    CHECK(pct == 0 || pct < 1.0f);

    // a block covering about 10 % of the image changes a lot
    Frame moved(320, 180, 320, 100, 128, 128);
    for (int row = 0; row < 60; ++row)
        for (int c = 0; c < 96; ++c) moved.data[(size_t)row * 320 + c] = 220;
    md.process(a.v.y, 320, 180, 320, 3, &pct);               // reset the reference
    CHECK(md.process(moved.v.y, 320, 180, 320, 3, &pct));
    CHECK(pct > 5.0f && pct < 15.0f);
    CHECK(!md.process(moved.v.y, 320, 180, 320, 3, &pct));   // now it is still again

    // threshold respected
    md.process(a.v.y, 320, 180, 320, 3, &pct);
    CHECK(!md.process(moved.v.y, 320, 180, 320, 50, &pct));  // 10 % change, 50 % threshold
    CHECK(!md.process(nullptr, 320, 180, 320, 3, &pct));     // null / tiny input is safe
    CHECK(!md.process(a.v.y, 4, 4, 320, 3, &pct));
}

int main() {
    testLetterbox();
    testConversion();
    testMotion();
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
