#include "detector.h"

#include <time.h>

#include <cstring>

#include "preprocess.h"
#include "yolov5.h"  // third_party/rockchip: rknn_app_context_t, init/inference helpers
#include "postprocess.h"

namespace aid {

namespace {
double nowMs() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
}  // namespace

struct Detector::Impl {
    rknn_app_context_t ctx;
    bool loaded = false;
    bool postReady = false;
    Letterbox lb;
    bool havePre = false;
};

Detector::Detector() : impl_(new Impl) { memset(&impl_->ctx, 0, sizeof(impl_->ctx)); }

Detector::~Detector() {
    if (impl_->postReady) deinit_post_process();
    if (impl_->loaded) release_yolov5_model(&impl_->ctx);
}

bool Detector::init(const std::string& modelPath, const std::string& labelsPath, std::string* err) {
    std::string dummy;
    if (!err) err = &dummy;
    if (impl_->loaded) return true;
    if (init_yolov5_model(modelPath.c_str(), &impl_->ctx) != 0) {
        *err = "cannot load the RKNN model " + modelPath + " (missing file, or the model does not match "
               "the board's RKNN runtime 1.6.0)";
        return false;
    }
    impl_->loaded = true;
    if (init_post_process(labelsPath.c_str()) != 0) {
        *err = "cannot read the labels file " + labelsPath;
        return false;   // the destructor releases the model
    }
    impl_->postReady = true;
    return true;
}

bool Detector::ready() const { return impl_->loaded && impl_->postReady; }
int Detector::inputWidth() const { return impl_->ctx.model_width; }
int Detector::inputHeight() const { return impl_->ctx.model_height; }

bool Detector::preprocess(const FrameView& f, DetectTiming* t) {
    if (!ready() || !impl_->ctx.input_mems[0] || impl_->ctx.model_channel != 3) return false;
    const double t0 = nowMs();
    uint8_t* dst = (uint8_t*)impl_->ctx.input_mems[0]->virt_addr;
    impl_->lb = nv12ToLetterboxRgb(f, dst, impl_->ctx.model_width, impl_->ctx.model_height);
    impl_->havePre = impl_->lb.newW > 0;
    if (t) t->preprocessMs = nowMs() - t0;
    return impl_->havePre;
}

bool Detector::infer(float confThreshold, std::vector<Detection>* out, DetectTiming* t, std::string* err) {
    std::string dummy;
    if (!err) err = &dummy;
    out->clear();
    if (!ready() || !impl_->havePre) { *err = "detector not ready"; return false; }
    const double t0 = nowMs();
    if (rknn_run(impl_->ctx.rknn_ctx, nullptr) < 0) { *err = "rknn_run failed"; return false; }
    const double t1 = nowMs();

    object_detect_result_list res;
    memset(&res, 0, sizeof(res));
    post_process(&impl_->ctx, impl_->ctx.output_mems, confThreshold, (float)NMS_THRESH, &res);
    const double t2 = nowMs();
    if (t) { t->inferMs = t1 - t0; t->postMs = t2 - t1; }

    for (int i = 0; i < res.count; ++i) {
        const object_detect_result& r = res.results[i];
        Detection d;
        d.cls = r.cls_id;
        d.label = coco_cls_to_name(r.cls_id);
        d.conf = r.prop;
        boxToNormalized(impl_->lb, r.box.left, r.box.top, r.box.right, r.box.bottom, &d.x1, &d.y1, &d.x2, &d.y2);
        out->push_back(d);
    }
    impl_->havePre = false;
    return true;
}

}  // namespace aid
