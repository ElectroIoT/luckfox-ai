cmake_minimum_required(VERSION 3.10)

# Cross-build for the Luckfox Pico (RV1106, uClibc) with the compiler from the Luckfox SDK.
# Needs two environment variables (see scripts/build.sh, which sets defaults inside WSL):
#   LUCKFOX_SDK_PATH      the luckfox-pico SDK checkout (compiler)
#   LUCKFOX_RKMPI_PATH    the luckfox_pico_rkmpi_example checkout (Rockchip headers + libs)
if(NOT DEFINED ENV{LUCKFOX_SDK_PATH} OR NOT DEFINED ENV{LUCKFOX_RKMPI_PATH})
    message(FATAL_ERROR "Set LUCKFOX_SDK_PATH and LUCKFOX_RKMPI_PATH (use scripts/build.sh)")
endif()
set(SDK_PATH   $ENV{LUCKFOX_SDK_PATH})
set(RKMPI_PATH $ENV{LUCKFOX_RKMPI_PATH})
set(TC "${SDK_PATH}/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf")
set(CMAKE_C_COMPILER   "${TC}-gcc")
set(CMAKE_CXX_COMPILER "${TC}-g++")

project(luckfox-ai C CXX)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Only RV1106_1103 is defined on purpose: the Luckfox example lists more -D flags, but its
# CMake file adds them after add_executable(), so they never applied and its (working) build
# used just this one. -DROCKIVA, for instance, needs headers that are not shipped with it.
add_compile_options(-g -Wall -Wextra -DRV1106_1103)

link_directories("${RKMPI_PATH}/lib/uclibc")
# The board keeps its media libraries in /oem/usr/lib; use that at run time.
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath,/oem/usr/lib -Wl,-rpath-link,${RKMPI_PATH}/lib/uclibc")

set(RK_INCLUDES
    "${RKMPI_PATH}/include"
    "${RKMPI_PATH}/include/rknn"
    "${RKMPI_PATH}/include/librga"
    "${RKMPI_PATH}/include/rkaiq"
    "${RKMPI_PATH}/include/rkaiq/uAPI2"
    "${RKMPI_PATH}/include/rkaiq/common"
    "${RKMPI_PATH}/include/rkaiq/xcore"
    "${RKMPI_PATH}/include/rkaiq/algos"
    "${RKMPI_PATH}/include/rkaiq/iq_parser"
    "${RKMPI_PATH}/include/rkaiq/iq_parser_v2"
    "${RKMPI_PATH}/include/rkaiq/smartIr")

set(RK_LIBS rockiva sample_comm rockit rockchip_mpp rkaiq rga pthread)

# ---- modules (one static library each)
add_library(aid_system STATIC src/system/config.cc src/system/sysinfo.cc src/system/json_util.cc src/system/log.cc)
target_include_directories(aid_system PUBLIC src/system)

add_library(aid_camera STATIC src/camera/camera.cc)
target_include_directories(aid_camera PUBLIC src/camera ${RK_INCLUDES})
target_link_libraries(aid_camera PUBLIC ${RK_LIBS})

add_library(aid_streaming STATIC src/streaming/streamer.cc)
target_include_directories(aid_streaming PUBLIC src/streaming ${RK_INCLUDES})
target_link_libraries(aid_streaming PUBLIC aid_camera rtsp)

add_library(aid_events STATIC src/events/event_engine.cc src/events/event_store.cc)
target_include_directories(aid_events PUBLIC src/events)
target_link_libraries(aid_events PUBLIC aid_system)

# Rockchip's YOLOv5 demo code (Apache-2.0, see its NOTICE.md): compiled without warnings.
set(ROCKCHIP_SRC src/ai/third_party/rockchip/yolov5.cc src/ai/third_party/rockchip/postprocess.cc)
set_source_files_properties(${ROCKCHIP_SRC} PROPERTIES COMPILE_FLAGS "-w")
add_library(aid_ai STATIC src/ai/detector.cc src/ai/detection_service.cc src/ai/preprocess.cc src/ai/motion.cc ${ROCKCHIP_SRC})
target_include_directories(aid_ai PUBLIC src/ai src/ai/third_party/rockchip src/camera ${RK_INCLUDES})
target_link_libraries(aid_ai PUBLIC aid_events aid_system rknnmrt pthread)

add_library(aid_webserver STATIC src/webserver/http_server.cc src/webserver/auth.cc src/webserver/api.cc)
target_include_directories(aid_webserver PUBLIC src/webserver)
target_link_libraries(aid_webserver PUBLIC aid_events aid_system pthread)

add_library(aid_mqtt STATIC src/mqtt/mqtt_client.cc src/mqtt/ha_discovery.cc)
target_include_directories(aid_mqtt PUBLIC src/mqtt)
target_link_libraries(aid_mqtt PUBLIC aid_system pthread)

add_library(aid_gpio STATIC src/gpio/gpio.cc)
target_include_directories(aid_gpio PUBLIC src/gpio)
target_link_libraries(aid_gpio PUBLIC aid_system)

# ---- programs
add_executable(aid-camera src/camera/aid_camera_cli.cc)
target_link_libraries(aid-camera PRIVATE aid_camera)

add_executable(aid-core src/core/aid_core.cc)
target_link_libraries(aid-core PRIVATE aid_system aid_camera aid_streaming aid_events aid_ai aid_webserver aid_mqtt aid_gpio)

add_executable(aid-supervisor src/core/aid_supervisor.cc)

install(TARGETS aid-camera aid-core aid-supervisor DESTINATION bin)
