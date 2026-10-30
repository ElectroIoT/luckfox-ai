#include "camera.h"

#include <dirent.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include "sample_comm.h"

namespace aid {

namespace {

constexpr int kViDev = 0;
constexpr int kViChn = 0;
constexpr int kJpegChn = 0;
constexpr int kFirstFrameMs = 5000;  // the sensor can take a while to start streaming

uint64_t nowUs() {
    struct timespec t = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000ull;
}

std::string hex(int v) {
    char b[24];
    snprintf(b, sizeof(b), "0x%x", (unsigned)v);
    return b;
}

// The following two functions follow the Luckfox / Rockchip samples.
bool viDevInit(std::string* err) {
    VI_DEV_ATTR_S devAttr;
    VI_DEV_BIND_PIPE_S bindPipe;
    memset(&devAttr, 0, sizeof(devAttr));
    memset(&bindPipe, 0, sizeof(bindPipe));

    int ret = RK_MPI_VI_GetDevAttr(kViDev, &devAttr);
    if (ret == RK_ERR_VI_NOT_CONFIG) {
        ret = RK_MPI_VI_SetDevAttr(kViDev, &devAttr);
        if (ret != RK_SUCCESS) {
            *err = "RK_MPI_VI_SetDevAttr " + hex(ret);
            return false;
        }
    }
    ret = RK_MPI_VI_GetDevIsEnable(kViDev);
    if (ret != RK_SUCCESS) {
        ret = RK_MPI_VI_EnableDev(kViDev);
        if (ret != RK_SUCCESS) {
            *err = "RK_MPI_VI_EnableDev " + hex(ret);
            return false;
        }
        bindPipe.u32Num = 1;
        bindPipe.PipeId[0] = kViDev;
        ret = RK_MPI_VI_SetDevBindPipe(kViDev, &bindPipe);
        if (ret != RK_SUCCESS) {
            *err = "RK_MPI_VI_SetDevBindPipe " + hex(ret);
            return false;
        }
    }
    return true;
}

bool viChnInit(int width, int height, std::string* err) {
    VI_CHN_ATTR_S attr;
    memset(&attr, 0, sizeof(attr));
    attr.stIspOpt.u32BufCount = 2;
    attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    attr.stSize.u32Width = width;
    attr.stSize.u32Height = height;
    attr.enPixelFormat = RK_FMT_YUV420SP;
    attr.enCompressMode = COMPRESS_MODE_NONE;
    attr.u32Depth = 2;  // lets us fetch frames with RK_MPI_VI_GetChnFrame
    int ret = RK_MPI_VI_SetChnAttr(kViDev, kViChn, &attr);
    if (ret != RK_SUCCESS) {
        *err = "RK_MPI_VI_SetChnAttr " + hex(ret);
        return false;
    }
    ret = RK_MPI_VI_EnableChn(kViDev, kViChn);
    if (ret != RK_SUCCESS) {
        *err = "RK_MPI_VI_EnableChn " + hex(ret);
        return false;
    }
    return true;
}

XCamReturn ispErrCb(rk_aiq_err_msg_t*) { return XCAM_RETURN_NO_ERROR; }
XCamReturn ispSofCb(rk_aiq_metas_t*) { return XCAM_RETURN_NO_ERROR; }

bool levelOk(int v) { return v >= 0 && v <= 255; }

std::string readFirstLine(const std::string& path) {
    std::ifstream f(path.c_str());
    std::string s;
    if (f) std::getline(f, s);
    return s;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) o += ' ';
        else o += c;
    }
    return o;
}

bool processRunning(const char* comm) {
    DIR* d = opendir("/proc");
    if (!d) return false;
    bool found = false;
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        std::string c = readFirstLine(std::string("/proc/") + e->d_name + "/comm");
        if (c == comm) { found = true; break; }
    }
    closedir(d);
    return found;
}

}  // namespace

struct Camera::Impl {
    CameraConfig cfg;
    bool open = false;
    bool ispUp = false;
    rk_aiq_sys_ctx_t* aiq = nullptr;  // our own ISP (rkaiq) context; the SDK's one is private
    bool sysUp = false;
    bool viDevUp = false;
    bool viChnUp = false;
    bool jpegUp = false;
};

Camera::Camera() : impl_(new Impl) {}
Camera::~Camera() { close(); }
bool Camera::isOpen() const { return impl_->open; }

bool Camera::open(const CameraConfig& cfg, std::string* err) {
    std::string dummy;
    if (!err) err = &dummy;
    if (impl_->open) return true;
    impl_->cfg = cfg;

    if (processRunning("rkipc")) {
        *err = "stock rkipc is running and owns the camera; run scripts/stop_stock_camera.sh first";
        return false;
    }

    // ISP (auto exposure / white balance etc.). Same calls as the Luckfox/Rockchip sample
    // SAMPLE_COMM_ISP_Init/Run, but we keep the context so we can change image controls.
    setenv("HDR_MODE", "0", 1);  // normal (non-HDR) mode, as in the sample
    rk_aiq_static_info_t info;
    memset(&info, 0, sizeof(info));
    if (rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(0, &info) != 0) {
        *err = "rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId failed (no sensor?)";
        return false;
    }
    rk_aiq_uapi2_sysctl_preInit_devBufCnt(info.sensor_info.sensor_name, "rkraw_rx", 2);
    impl_->aiq = rk_aiq_uapi2_sysctl_init(info.sensor_info.sensor_name, cfg.iqDir.c_str(),
                                          ispErrCb, ispSofCb);
    if (!impl_->aiq) {
        *err = "rk_aiq_uapi2_sysctl_init failed (iq dir " + cfg.iqDir + ")";
        return false;
    }
    if (rk_aiq_uapi2_sysctl_prepare(impl_->aiq, 0, 0, RK_AIQ_WORKING_MODE_NORMAL) != 0) {
        *err = "rk_aiq_uapi2_sysctl_prepare failed";
        rk_aiq_uapi2_sysctl_deinit(impl_->aiq);
        impl_->aiq = nullptr;
        return false;
    }
    if (rk_aiq_uapi2_sysctl_start(impl_->aiq) != 0) {
        *err = "rk_aiq_uapi2_sysctl_start failed";
        rk_aiq_uapi2_sysctl_deinit(impl_->aiq);
        impl_->aiq = nullptr;
        return false;
    }
    impl_->ispUp = true;

    if (cfg.fps > 0) {
        Uapi_ExpSwAttrV2_t sw;
        memset(&sw, 0, sizeof(sw));
        if (rk_aiq_user_api2_ae_getExpSwAttr(impl_->aiq, &sw) == 0) {
            sw.stAuto.stFrmRate.isFpsFix = true;
            sw.stAuto.stFrmRate.FpsValue = (unsigned)cfg.fps;
            rk_aiq_user_api2_ae_setExpSwAttr(impl_->aiq, sw);
        }
    }
    RK_S32 ret;
    ret = RK_MPI_SYS_Init();
    if (ret != RK_SUCCESS) {
        *err = "RK_MPI_SYS_Init " + hex(ret);
        close();
        return false;
    }
    impl_->sysUp = true;

    if (!viDevInit(err)) { close(); return false; }
    impl_->viDevUp = true;
    if (!viChnInit(cfg.width, cfg.height, err)) { close(); return false; }
    impl_->viChnUp = true;

    impl_->open = true;
    if (!setControls(cfg.controls, err)) { close(); return false; }
    return true;
}

void Camera::close() {
    if (impl_->jpegUp) {
        RK_MPI_VENC_StopRecvFrame(kJpegChn);
        RK_MPI_VENC_DestroyChn(kJpegChn);
        impl_->jpegUp = false;
    }
    if (impl_->viChnUp) { RK_MPI_VI_DisableChn(kViDev, kViChn); impl_->viChnUp = false; }
    if (impl_->viDevUp) { RK_MPI_VI_DisableDev(kViDev); impl_->viDevUp = false; }
    if (impl_->ispUp) {
        rk_aiq_uapi2_sysctl_stop(impl_->aiq, false);
        rk_aiq_uapi2_sysctl_deinit(impl_->aiq);
        impl_->aiq = nullptr;
        impl_->ispUp = false;
    }
    if (impl_->sysUp) { RK_MPI_SYS_Exit(); impl_->sysUp = false; }
    impl_->open = false;
}

bool Camera::setControls(const ImageControls& c, std::string* err) {
    std::string dummy;
    if (!err) err = &dummy;
    if (!impl_->ispUp || !impl_->aiq) { *err = "ISP not running"; return false; }
    struct Item { const char* name; int v; };
    const Item levels[] = {{"brightness", c.brightness}, {"contrast", c.contrast},
                           {"saturation", c.saturation}, {"sharpness", c.sharpness}};
    for (const Item& it : levels)
        if (it.v != -1 && !levelOk(it.v)) { *err = std::string(it.name) + " must be 0..255"; return false; }

    XCamReturn r = XCAM_RETURN_NO_ERROR;
    if (c.brightness != -1) r = rk_aiq_uapi2_setBrightness(impl_->aiq, (unsigned)c.brightness);
    if (r == XCAM_RETURN_NO_ERROR && c.contrast != -1) r = rk_aiq_uapi2_setContrast(impl_->aiq, (unsigned)c.contrast);
    if (r == XCAM_RETURN_NO_ERROR && c.saturation != -1) r = rk_aiq_uapi2_setSaturation(impl_->aiq, (unsigned)c.saturation);
    if (r == XCAM_RETURN_NO_ERROR && c.sharpness != -1) r = rk_aiq_uapi2_setSharpness(impl_->aiq, (unsigned)c.sharpness);
    if (r == XCAM_RETURN_NO_ERROR && c.aeLock != -1) r = rk_aiq_uapi2_setAeLock(impl_->aiq, c.aeLock == 1);
    if (r != XCAM_RETURN_NO_ERROR) { *err = "rkaiq call failed, code " + std::to_string((int)r); return false; }
    return true;
}

std::string Camera::controlsJson() {
    std::ostringstream o;
    o << "{";
    if (!impl_->ispUp || !impl_->aiq) { o << "\"error\": \"ISP not running\"}"; return o.str(); }
    unsigned v = 0;
    auto lvl = [&](const char* name, XCamReturn (*get)(const rk_aiq_sys_ctx_t*, unsigned int*), bool first) {
        v = 0;
        const XCamReturn r = get(impl_->aiq, &v);
        if (!first) o << ", ";
        o << "\"" << name << "\": ";
        if (r == XCAM_RETURN_NO_ERROR) o << v; else o << "null";
    };
    lvl("brightness", rk_aiq_uapi2_getBrightness, true);
    lvl("contrast", rk_aiq_uapi2_getContrast, false);
    lvl("saturation", rk_aiq_uapi2_getSaturation, false);
    lvl("sharpness", rk_aiq_uapi2_getSharpness, false);
    opMode_t mode = OP_INVAL;
    if (rk_aiq_uapi2_getExpMode(impl_->aiq, &mode) == XCAM_RETURN_NO_ERROR)
        o << ", \"exposure_mode\": \"" << (mode == OP_AUTO ? "auto" : (mode == OP_MANUAL ? "manual" : "other")) << "\"";
    paRange_t t = {0, 0}, g = {0, 0};
    if (rk_aiq_uapi2_getExpTimeRange(impl_->aiq, &t) == XCAM_RETURN_NO_ERROR)
        o << ", \"exposure_time_range_s\": [" << t.min << ", " << t.max << "]";
    if (rk_aiq_uapi2_getExpGainRange(impl_->aiq, &g) == XCAM_RETURN_NO_ERROR)
        o << ", \"gain_range\": [" << g.min << ", " << g.max << "]";
    o << "}";
    return o.str();
}

bool Camera::measureFps(double seconds, CameraStats* out, std::string* err) {
    std::string dummy;
    if (!err) err = &dummy;
    if (!impl_->open) { *err = "camera not open"; return false; }

    CameraStats st;
    st.width = impl_->cfg.width;
    st.height = impl_->cfg.height;
    uint64_t first = 0, last = 0;
    const uint64_t deadline = nowUs() + (uint64_t)(seconds * 1e6);

    while (nowUs() < deadline) {
        VIDEO_FRAME_INFO_S frame;
        memset(&frame, 0, sizeof(frame));
        RK_S32 ret = RK_MPI_VI_GetChnFrame(kViDev, kViChn, &frame, st.frames == 0 ? kFirstFrameMs : 1000);
        if (ret != RK_SUCCESS) {
            if (st.frames == 0) { *err = "no frame within first-frame timeout: RK_MPI_VI_GetChnFrame " + hex(ret); return false; }
            continue;
        }
        const uint64_t t = nowUs();
        if (st.frames == 0) first = t;
        last = t;
        st.frames++;
        RK_MPI_VI_ReleaseChnFrame(kViDev, kViChn, &frame);
    }
    if (st.frames >= 2 && last > first) {
        st.seconds = (last - first) / 1e6;
        st.fps = (double)(st.frames - 1) / st.seconds;
    }
    if (out) *out = st;
    return true;
}

bool Camera::snapshotJpeg(const std::string& path, int quality, SnapshotInfo* out, std::string* err) {
    std::string dummy;
    if (!err) err = &dummy;
    if (!impl_->open) { *err = "camera not open"; return false; }
    const int w = impl_->cfg.width, h = impl_->cfg.height;

    if (!impl_->jpegUp) {
        VENC_CHN_ATTR_S attr;
        memset(&attr, 0, sizeof(attr));
        attr.stVencAttr.enType = RK_VIDEO_ID_JPEG;
        attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
        attr.stVencAttr.u32PicWidth = w;
        attr.stVencAttr.u32PicHeight = h;
        attr.stVencAttr.u32VirWidth = w;
        attr.stVencAttr.u32VirHeight = h;
        attr.stVencAttr.u32StreamBufCnt = 2;
        attr.stVencAttr.u32BufSize = w * h * 3 / 2;
        attr.stVencAttr.enMirror = MIRROR_NONE;
        attr.stVencAttr.stAttrJpege.bSupportDCF = RK_FALSE;
        attr.stVencAttr.stAttrJpege.stMPFCfg.u8LargeThumbNailNum = 0;
        attr.stVencAttr.stAttrJpege.enReceiveMode = VENC_PIC_RECEIVE_SINGLE;
        RK_S32 ret = RK_MPI_VENC_CreateChn(kJpegChn, &attr);
        if (ret != RK_SUCCESS) { *err = "RK_MPI_VENC_CreateChn(JPEG) " + hex(ret); return false; }
        impl_->jpegUp = true;
    }

    VENC_JPEG_PARAM_S jp;
    memset(&jp, 0, sizeof(jp));
    jp.u32Qfactor = (RK_U32)(quality < 1 ? 1 : (quality > 99 ? 99 : quality));
    RK_MPI_VENC_SetJpegParam(kJpegChn, &jp);

    // Discard a few frames first: right after start-up the auto exposure / white balance (3A)
    // is still converging, so the first frames can be too dark or tinted. 10 is a guess;
    // tune it after looking at real snapshots.
    constexpr int kWarmupFrames = 10;
    VIDEO_FRAME_INFO_S frame;
    RK_S32 ret = RK_FAILURE;
    for (int i = 0; i <= kWarmupFrames; ++i) {
        memset(&frame, 0, sizeof(frame));
        ret = RK_MPI_VI_GetChnFrame(kViDev, kViChn, &frame, i == 0 ? kFirstFrameMs : 2000);
        if (ret != RK_SUCCESS) { *err = "RK_MPI_VI_GetChnFrame " + hex(ret); return false; }
        if (i < kWarmupFrames) RK_MPI_VI_ReleaseChnFrame(kViDev, kViChn, &frame);
    }

    bool ok = false;
    VENC_RECV_PIC_PARAM_S recv;
    memset(&recv, 0, sizeof(recv));
    recv.s32RecvPicNum = 1;
    ret = RK_MPI_VENC_StartRecvFrame(kJpegChn, &recv);
    if (ret != RK_SUCCESS) {
        *err = "RK_MPI_VENC_StartRecvFrame " + hex(ret);
    } else {
        const uint64_t t0 = nowUs();
        ret = RK_MPI_VENC_SendFrame(kJpegChn, &frame, 1000);
        if (ret != RK_SUCCESS) {
            *err = "RK_MPI_VENC_SendFrame " + hex(ret);
        } else {
            VENC_STREAM_S stream;
            memset(&stream, 0, sizeof(stream));
            VENC_PACK_S pack;
            memset(&pack, 0, sizeof(pack));
            stream.pstPack = &pack;
            ret = RK_MPI_VENC_GetStream(kJpegChn, &stream, 1000);
            if (ret != RK_SUCCESS) {
                *err = "RK_MPI_VENC_GetStream " + hex(ret);
            } else {
                const double ms = (nowUs() - t0) / 1000.0;
                const void* data = RK_MPI_MB_Handle2VirAddr(pack.pMbBlk);
                FILE* f = fopen(path.c_str(), "wb");
                if (!f) {
                    *err = "cannot open " + path + " for writing";
                } else {
                    const size_t n = fwrite(data, 1, pack.u32Len, f);
                    fclose(f);
                    if (n != pack.u32Len) {
                        *err = "short write to " + path;
                    } else {
                        ok = true;
                        if (out) {
                            out->bytes = pack.u32Len;
                            out->width = w;
                            out->height = h;
                            out->encodeMs = ms;
                        }
                    }
                }
                RK_MPI_VENC_ReleaseStream(kJpegChn, &stream);
            }
        }
    }
    RK_MPI_VI_ReleaseChnFrame(kViDev, kViChn, &frame);
    return ok;
}

std::string Camera::probeStatusJson() {
    // Sensor sub-device names look like "m00_b_sc3336 4-0030"; discover, do not assume.
    std::string sensor;
    std::vector<std::string> nodes;
    if (DIR* d = opendir("/sys/class/video4linux")) {
        while (struct dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::string n = readFirstLine(std::string("/sys/class/video4linux/") + e->d_name + "/name");
            if (n.size() > 3 && n[0] == 'm' && n[1] >= '0' && n[1] <= '9' && n[3] == '_')
                sensor = n;
            nodes.push_back(e->d_name);
        }
        closedir(d);
    }
    bool rkipc = processRunning("rkipc");

    std::ostringstream o;
    o << "{\n";
    o << "  \"sensor\": \"" << jsonEscape(sensor) << "\",\n";
    o << "  \"sensor_detected\": " << (sensor.empty() ? "false" : "true") << ",\n";
    o << "  \"v4l_nodes\": " << nodes.size() << ",\n";
    o << "  \"stock_rkipc_running\": " << (rkipc ? "true" : "false") << ",\n";
    o << "  \"camera_available_to_us\": " << ((!sensor.empty() && !rkipc) ? "true" : "false") << "\n";
    o << "}\n";
    return o.str();
}

}  // namespace aid
