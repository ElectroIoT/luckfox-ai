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
add_library(aid_system STATIC src/system/config.cc src/system/sysinfo.cc)
target_include_directories(aid_system PUBLIC src/system)

add_library(aid_camera STATIC src/camera/camera.cc)
target_include_directories(aid_camera PUBLIC src/camera ${RK_INCLUDES})
target_link_libraries(aid_camera PUBLIC ${RK_LIBS})

add_library(aid_streaming STATIC src/streaming/streamer.cc)
target_include_directories(aid_streaming PUBLIC src/streaming ${RK_INCLUDES})
target_link_libraries(aid_streaming PUBLIC aid_camera rtsp)

add_library(aid_webserver STATIC src/webserver/http_server.cc)
target_include_directories(aid_webserver PUBLIC src/webserver)

# ---- programs
add_executable(aid-camera src/camera/aid_camera_cli.cc)
target_link_libraries(aid-camera PRIVATE aid_camera)

add_executable(aid-core src/core/aid_core.cc)
target_link_libraries(aid-core PRIVATE aid_system aid_camera aid_streaming aid_webserver)

install(TARGETS aid-camera aid-core DESTINATION bin)
