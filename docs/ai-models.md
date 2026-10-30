# AI models

## What runs on the board
- **Model:** the `yolov5.rknn` that ships with Luckfox's `luckfox_pico_rkmpi_example` (YOLOv5, COCO 80 classes, 640x640 input, INT8). I do not know which YOLOv5 size it is (n, s or s_relu): the file does not say. It was not converted by us.
- **Runtime on the board:** `librknnmrt` version **1.6.0** (read from the board). Models must be converted with RKNN-Toolkit2 **1.6.0** (the version the Luckfox wiki uses) or a version known to produce files that runtime accepts.
- **Where the code is:** `src/ai/third_party/rockchip/` (Rockchip's YOLOv5 demo, Apache-2.0, labels path made a parameter; see its `NOTICE.md`), `src/ai/detector.cc` (our wrapper), `src/ai/preprocess.cc` (camera frame to model input), `src/ai/detection_service.cc` (timer loop).
- The model is **not stored in this repository** (7.6 MB binary). `scripts/deploy.sh` copies it from your `luckfox_pico_rkmpi_example` checkout to `/root/aid/model/` on the board.

## How a picture becomes detections
1. Every `detect_interval_ms` (default 500 ms) one camera frame (NV12, 1920x1080) is borrowed from the camera pipeline.
2. The CPU converts it to RGB, shrinks it to 640x360 and puts it into a 640x640 input with grey bars top and bottom (letterbox). Nearest-neighbour sampling. The frame is released right after.
3. The NPU runs the network.
4. The CPU decodes boxes and removes overlaps (NMS, threshold 0.45). Boxes are mapped back to the full picture (0 to 1).
5. The event engine filters: class list, minimum confidence, detection zones; then applies confirmation frames, cooldowns and "object disappeared" timing.

## Measured on the board (one board, one scene, about 1 h in total, `status.json` counters)
| What | Value |
|---|---|
| NPU inference (`rknn_run`) | **about 89 ms** average over 600+ runs |
| Picture preparation (CPU) | 34 to 39 ms |
| Box decoding + NMS (CPU) | about 2 ms |
| Whole pass | about 125 ms, so 500 ms interval = about 2 detections per second |
| `aid-core` CPU use with detection every 500 ms and one viewer | 11 to 26 % of the single core (several samples) |
| Free RAM with everything running | about 28 MB |

Not measured: accuracy (precision/recall) of any kind, behaviour at night, behaviour with many objects, shorter intervals than 500 ms, the NPU's own load (the debug file for it does not exist on this kernel).
In the live scene the detector reported objects such as a "suitcase"; I did not check whether such detections were correct. Expect false positives from a small INT8 model, especially at low confidence. Raise `detect_conf_pct` or use zones if they bother you.

## Which other models are possible (from the Rockchip model zoo README, not tested here)
The RKNN Model Zoo marks RV1103/RV1106 as "limited support". Models it lists for those chips: yolov5 (n, s_relu, s, m), yolov10 (n, s), yolo11 (n, s, m), yolo26 (n, s, m), mobilenet (classification), LPRNet. **Not** listed for RV1106: yolov8, yolov6/7, yolox, ppyoloe. MobileNet-SSD is not in the zoo README.
No RV1106 speed figures are published; measure on the board. A model must also be accepted by the board's runtime 1.6.0: newer converter output may fail with "incompatible model version". `yolo11n` was not tried and is the most interesting next candidate.

## Converting your own model (not done in this project)
Needs Ubuntu 22.04 (WSL2 works), Python 3.10 and RKNN-Toolkit2 1.6.0. In outline: install the toolkit from `https://github.com/rockchip-linux/rknn-toolkit2`, take an ONNX model from the zoo links, run the zoo's `convert.py` with target `rv1106` and INT8 quantization using a calibration image list, copy the `.rknn` to `/root/aid/model/`, and set `model_path`. The post-processing in `third_party/rockchip/postprocess.cc` is written for the YOLOv5 output layout (3 outputs, anchors); other model families need their own post-processing.

## Zones, classes, events
- Classes the dashboard offers: person, car, motorcycle, bicycle, bus, truck, dog, cat, bird, backpack, handbag, suitcase (any COCO name works in the config file).
- Event groups: person; vehicle (car, motorcycle, bus, truck); animal (dog, cat); other classes use their own name.
- `event_enter_frames` (default 2) detections in a row confirm an object; `event_cooldown_s` (30) limits repeats per type; `event_absent_s` (10) is how long an object must be gone before "object disappeared".
