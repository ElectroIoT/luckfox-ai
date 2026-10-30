// YOLOv5 object detection on the RV1106 NPU (RKNN runtime 1.6.0 on the board).
// Wraps the Rockchip demo code in third_party/rockchip. Board only: the NPU is not available on
// the PC, so this file is cross-compiled but cannot be unit tested there.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "event_engine.h"  // aid::Detection
#include "frame_view.h"

namespace aid {

struct DetectTiming {
    double preprocessMs = 0;  // NV12 -> letterboxed RGB on the CPU
    double inferMs = 0;       // rknn_run (NPU)
    double postMs = 0;        // decoding boxes + NMS on the CPU
};

class Detector {
public:
    Detector();
    ~Detector();
    Detector(const Detector&) = delete;
    Detector& operator=(const Detector&) = delete;

    bool init(const std::string& modelPath, const std::string& labelsPath, std::string* err);
    bool ready() const;
    int inputWidth() const;
    int inputHeight() const;

    // Step 1 (needs the camera frame): fills the model input buffer from the frame.
    bool preprocess(const FrameView& f, DetectTiming* t);
    // Step 2 (frame no longer needed): runs the NPU on the prepared input and returns the
    // detections above confThreshold (0..1), with boxes normalized to the preprocessed frame.
    bool infer(float confThreshold, std::vector<Detection>* out, DetectTiming* t, std::string* err);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aid
