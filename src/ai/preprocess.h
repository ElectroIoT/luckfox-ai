// Camera frame (NV12) -> model input (RGB, letterboxed) on the CPU, and box mapping back.
// Pure functions: unit-tested on the PC. Nearest-neighbour sampling keeps it cheap on one A7 core
// (a hardware RGA path may be faster; not tried yet).
#pragma once

#include <cstdint>

#include "frame_view.h"

namespace aid {

struct Letterbox {
    float scale = 1.0f;   // model pixels per frame pixel
    int padX = 0, padY = 0;
    int newW = 0, newH = 0;  // size of the image inside the model input
};

Letterbox computeLetterbox(int srcW, int srcH, int dstW, int dstH);

// Writes dstW*dstH*3 bytes (RGB, row after row, no padding between rows). Pixels outside the
// scaled image are set to `pad`. Returns the geometry used.
Letterbox nv12ToLetterboxRgb(const FrameView& f, uint8_t* dst, int dstW, int dstH, uint8_t pad = 114);

// Box in model pixels -> normalized 0..1 coordinates of the original frame (clamped).
void boxToNormalized(const Letterbox& lb, int left, int top, int right, int bottom, float* x1,
                     float* y1, float* x2, float* y2);

}  // namespace aid
