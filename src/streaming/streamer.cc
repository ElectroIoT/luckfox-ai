#include "streamer.h"

#include <time.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

#include "rtsp_demo.h"
#include "sample_comm.h"

namespace aid {

namespace {

constexpr int kH264Chn = 0;
constexpr int kJpegChn = 1;

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

}  // namespace

struct Streamer::Impl {
    StreamConfig cfg;
    bool started = false;
    bool h264Up = false;
    bool jpegUp = false;
    bool bound = false;

    rtsp_demo_handle rtsp = nullptr;
    rtsp_session_handle session = nullptr;

    std::atomic<bool> quit{false};
    std::thread thread;

    mutable std::mutex statsMutex;
    StreamStats stats;

    std::mutex snapMutex;

    MPP_CHN_S src, dstH264;

    void loop() {
        VENC_STREAM_S stream;
        VENC_PACK_S pack;
        memset(&stream, 0, sizeof(stream));
        memset(&pack, 0, sizeof(pack));
        stream.pstPack = &pack;

        uint64_t windowStart = nowUs();
        uint64_t winFrames = 0, winBytes = 0, winLatN = 0;
        double winLatSum = 0, winLatMax = -1;

        while (!quit) {
            const RK_S32 r = RK_MPI_VENC_GetStream(kH264Chn, &stream, 100);
            if (r == RK_SUCCESS) {
                const void* data = RK_MPI_MB_Handle2VirAddr(pack.pMbBlk);
                const uint64_t t = nowUs();
                rtsp_tx_video(session, (const uint8_t*)data, (int)pack.u32Len, pack.u64PTS);

                const double latMs = ((double)t - (double)pack.u64PTS) / 1000.0;
                {
                    std::lock_guard<std::mutex> lock(statsMutex);
                    stats.frames++;
                    stats.bytes += pack.u32Len;
                    if (pack.DataType.enH264EType == H264E_NALU_IDRSLICE) stats.keyframes++;
                }
                winFrames++;
                winBytes += pack.u32Len;
                // The capture timestamp is only trusted if it is plausible (same clock, < 5 s old).
                if (pack.u64PTS != 0 && latMs >= 0 && latMs < 5000) {
                    winLatN++;
                    winLatSum += latMs;
                    if (latMs > winLatMax) winLatMax = latMs;
                }
                RK_MPI_VENC_ReleaseStream(kH264Chn, &stream);
            }
            rtsp_do_event(rtsp);

            const uint64_t now = nowUs();
            if (now - windowStart >= 2000000ull) {
                const double sec = (now - windowStart) / 1e6;
                std::lock_guard<std::mutex> lock(statsMutex);
                stats.recentFps = winFrames / sec;
                stats.recentKbps = (winBytes * 8.0 / 1000.0) / sec;
                stats.latencyAvgMs = winLatN ? winLatSum / winLatN : -1;
                stats.latencyMaxMs = winLatN ? winLatMax : -1;
                windowStart = now;
                winFrames = winBytes = winLatN = 0;
                winLatSum = 0;
                winLatMax = -1;
            }
        }
    }
};

Streamer::Streamer() : impl_(new Impl) {}
Streamer::~Streamer() { stop(); }
bool Streamer::running() const { return impl_->started; }

StreamStats Streamer::stats() const {
    std::lock_guard<std::mutex> lock(impl_->statsMutex);
    StreamStats s = impl_->stats;
    s.running = impl_->started;
    return s;
}

bool Streamer::start(const StreamConfig& cfg, std::string* err) {
    std::string dummy;
    if (!err) err = &dummy;
    Impl& m = *impl_;
    if (m.started) return true;
    m.cfg = cfg;
    const int w = cfg.width, h = cfg.height;

    // ---- H.264 channel (hardware encoder)
    {
        VENC_CHN_ATTR_S a;
        memset(&a, 0, sizeof(a));
        a.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
        a.stRcAttr.stH264Cbr.u32BitRate = (RK_U32)cfg.bitrateKbps;
        a.stRcAttr.stH264Cbr.u32Gop = (RK_U32)cfg.gop;
        a.stRcAttr.stH264Cbr.u32SrcFrameRateNum = (RK_U32)cfg.fps;
        a.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
        a.stRcAttr.stH264Cbr.fr32DstFrameRateNum = (RK_U32)cfg.fps;
        a.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
        a.stVencAttr.enType = RK_VIDEO_ID_AVC;
        a.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
        a.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
        a.stVencAttr.u32PicWidth = w;
        a.stVencAttr.u32PicHeight = h;
        a.stVencAttr.u32VirWidth = w;
        a.stVencAttr.u32VirHeight = h;
        a.stVencAttr.u32StreamBufCnt = 2;
        a.stVencAttr.u32BufSize = w * h * 3 / 2;
        a.stVencAttr.enMirror = MIRROR_NONE;
        RK_S32 r = RK_MPI_VENC_CreateChn(kH264Chn, &a);
        if (r != RK_SUCCESS) { *err = "RK_MPI_VENC_CreateChn(H264) " + hex(r); return false; }
        m.h264Up = true;
    }

    // ---- JPEG channel: not bound; snapshot() sends it one frame pulled from VI
    {
        VENC_CHN_ATTR_S a;
        memset(&a, 0, sizeof(a));
        a.stVencAttr.enType = RK_VIDEO_ID_JPEG;
        a.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
        a.stVencAttr.u32PicWidth = w;
        a.stVencAttr.u32PicHeight = h;
        a.stVencAttr.u32VirWidth = w;
        a.stVencAttr.u32VirHeight = h;
        a.stVencAttr.u32StreamBufCnt = 2;
        a.stVencAttr.u32BufSize = w * h * 3 / 2;
        a.stVencAttr.enMirror = MIRROR_NONE;
        a.stVencAttr.stAttrJpege.bSupportDCF = RK_FALSE;
        a.stVencAttr.stAttrJpege.stMPFCfg.u8LargeThumbNailNum = 0;
        a.stVencAttr.stAttrJpege.enReceiveMode = VENC_PIC_RECEIVE_SINGLE;
        RK_S32 r = RK_MPI_VENC_CreateChn(kJpegChn, &a);
        if (r != RK_SUCCESS) { *err = "RK_MPI_VENC_CreateChn(JPEG) " + hex(r); stop(); return false; }
        m.jpegUp = true;
    }

    VENC_RECV_PIC_PARAM_S recv;
    memset(&recv, 0, sizeof(recv));
    recv.s32RecvPicNum = -1;  // H.264: receive continuously
    RK_S32 r = RK_MPI_VENC_StartRecvFrame(kH264Chn, &recv);
    if (r != RK_SUCCESS) { *err = "RK_MPI_VENC_StartRecvFrame(H264) " + hex(r); stop(); return false; }

    // ---- hardware bind: VI -> H.264
    memset(&m.src, 0, sizeof(m.src));
    memset(&m.dstH264, 0, sizeof(m.dstH264));
    m.src.enModId = RK_ID_VI;
    m.src.s32DevId = cfg.viDev;
    m.src.s32ChnId = cfg.viChn;
    m.dstH264.enModId = RK_ID_VENC;
    m.dstH264.s32DevId = 0;
    m.dstH264.s32ChnId = kH264Chn;
    r = RK_MPI_SYS_Bind(&m.src, &m.dstH264);
    if (r != RK_SUCCESS) { *err = "RK_MPI_SYS_Bind(VI->H264) " + hex(r); stop(); return false; }
    m.bound = true;

    // ---- RTSP server
    m.rtsp = create_rtsp_demo(cfg.rtspPort);
    if (!m.rtsp) {
        *err = "RTSP server could not listen on port " + std::to_string(cfg.rtspPort) +
               " (is the stock rkipc or another process using it?)";
        stop();
        return false;
    }
    m.session = rtsp_new_session(m.rtsp, cfg.rtspPath.c_str());
    if (!m.session) { *err = "rtsp_new_session failed for " + cfg.rtspPath; stop(); return false; }
    rtsp_set_video(m.session, RTSP_CODEC_ID_VIDEO_H264, NULL, 0);
    rtsp_sync_video_ts(m.session, rtsp_get_reltime(), rtsp_get_ntptime());

    m.quit = false;
    m.started = true;
    m.thread = std::thread([&m] { m.loop(); });
    return true;
}

void Streamer::stop() {
    Impl& m = *impl_;
    m.quit = true;
    if (m.thread.joinable()) m.thread.join();
    m.started = false;
    if (m.bound) {
        RK_MPI_SYS_UnBind(&m.src, &m.dstH264);
        m.bound = false;
    }
    if (m.h264Up) {
        RK_MPI_VENC_StopRecvFrame(kH264Chn);
        RK_MPI_VENC_DestroyChn(kH264Chn);
        m.h264Up = false;
    }
    if (m.jpegUp) {
        RK_MPI_VENC_StopRecvFrame(kJpegChn);
        RK_MPI_VENC_DestroyChn(kJpegChn);
        m.jpegUp = false;
    }
    if (m.rtsp) {
        rtsp_del_demo(m.rtsp);  // also frees its sessions
        m.rtsp = nullptr;
        m.session = nullptr;
    }
}

bool Streamer::snapshot(std::vector<uint8_t>* jpeg, std::string* err) {
    std::string dummy;
    if (!err) err = &dummy;
    Impl& m = *impl_;
    if (!m.started || !m.jpegUp) { *err = "stream not running"; return false; }
    std::lock_guard<std::mutex> lock(m.snapMutex);

    VENC_STREAM_S stream;
    VENC_PACK_S pack;
    memset(&stream, 0, sizeof(stream));
    memset(&pack, 0, sizeof(pack));
    stream.pstPack = &pack;

    // Drop any frame left over from an earlier snapshot that timed out.
    while (RK_MPI_VENC_GetStream(kJpegChn, &stream, 0) == RK_SUCCESS)
        RK_MPI_VENC_ReleaseStream(kJpegChn, &stream);

    VENC_JPEG_PARAM_S jp;
    memset(&jp, 0, sizeof(jp));
    jp.u32Qfactor = (RK_U32)m.cfg.jpegQuality;
    RK_MPI_VENC_SetJpegParam(kJpegChn, &jp);

    // A fresh frame from the camera channel (the H.264 encoder is bound to the same channel).
    VIDEO_FRAME_INFO_S frame;
    memset(&frame, 0, sizeof(frame));
    RK_S32 r = RK_MPI_VI_GetChnFrame(m.cfg.viDev, m.cfg.viChn, &frame, 2000);
    if (r != RK_SUCCESS) { *err = "RK_MPI_VI_GetChnFrame " + hex(r); return false; }

    VENC_RECV_PIC_PARAM_S recv;
    memset(&recv, 0, sizeof(recv));
    recv.s32RecvPicNum = 1;
    r = RK_MPI_VENC_StartRecvFrame(kJpegChn, &recv);
    if (r == RK_SUCCESS) r = RK_MPI_VENC_SendFrame(kJpegChn, &frame, 1000);
    RK_MPI_VI_ReleaseChnFrame(m.cfg.viDev, m.cfg.viChn, &frame);
    if (r != RK_SUCCESS) { *err = "JPEG encode request " + hex(r); return false; }

    r = RK_MPI_VENC_GetStream(kJpegChn, &stream, 2000);
    if (r != RK_SUCCESS) { *err = "GetStream(JPEG) " + hex(r); return false; }
    const uint8_t* data = (const uint8_t*)RK_MPI_MB_Handle2VirAddr(pack.pMbBlk);
    jpeg->assign(data, data + pack.u32Len);
    RK_MPI_VENC_ReleaseStream(kJpegChn, &stream);
    {
        std::lock_guard<std::mutex> sl(m.statsMutex);
        m.stats.snapshots++;
    }
    return true;
}

}  // namespace aid
