# Third-party code in this directory

`yolov5.cc`, `yolov5.h`, `postprocess.cc`, `postprocess.h` come from the Rockchip **rknn_model_zoo**
YOLOv5 C demo as shipped in the Luckfox `luckfox_pico_rkmpi_example` repository
(https://github.com/LuckfoxTECH/luckfox_pico_rkmpi_example). They are licensed under the
**Apache License, Version 2.0** (header of each file; https://www.apache.org/licenses/LICENSE-2.0).

Changes made for luckfox-ai: `init_post_process()` takes the path of the labels file as an argument
(it used a fixed relative path); a "MODIFIED" comment was added at the top of each file.
They are compiled without warnings enabled, because they are not our code.
