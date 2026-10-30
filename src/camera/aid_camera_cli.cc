// aid-camera: small command-line front end for the camera service (Phase 3).
//   aid-camera status                      read-only, works while rkipc runs
//   aid-camera fps [seconds] [opts]        measure the real frame rate
//   aid-camera snapshot <file.jpg> [opts]  save one JPEG
//   aid-camera controls [opts]             apply image controls and print the values read back
// opts: --width W --height H --fps N --quality Q
//       --brightness N --contrast N --saturation N --sharpness N   (0..255)
//       --ae-lock 0|1
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include "camera.h"

namespace {

void usage(const char* me) {
    fprintf(stderr,
            "usage:\n"
            "  %s status\n"
            "  %s fps [seconds] [--width W] [--height H] [--fps N]\n"
            "  %s snapshot <file.jpg> [--width W] [--height H] [--fps N] [--quality Q] [image opts]\n"
            "  %s controls [image opts]\n"
            "image opts: --brightness N --contrast N --saturation N --sharpness N (0..255) --ae-lock 0|1\n",
            me, me, me, me);
}

bool parseOpts(int argc, char** argv, int start, aid::CameraConfig* cfg, int* quality) {
    for (int i = start; i < argc; ++i) {
        const char* a = argv[i];
        if (i + 1 >= argc) { fprintf(stderr, "option %s needs a value\n", a); return false; }
        const int v = atoi(argv[++i]);
        if (!strcmp(a, "--width")) cfg->width = v;
        else if (!strcmp(a, "--height")) cfg->height = v;
        else if (!strcmp(a, "--fps")) cfg->fps = v;
        else if (!strcmp(a, "--quality")) *quality = v;
        else if (!strcmp(a, "--brightness")) cfg->controls.brightness = v;
        else if (!strcmp(a, "--contrast")) cfg->controls.contrast = v;
        else if (!strcmp(a, "--saturation")) cfg->controls.saturation = v;
        else if (!strcmp(a, "--sharpness")) cfg->controls.sharpness = v;
        else if (!strcmp(a, "--ae-lock")) cfg->controls.aeLock = v ? 1 : 0;
        else { fprintf(stderr, "unknown option %s\n", a); return false; }
    }
    if (cfg->width < 32 || cfg->height < 32 || cfg->width > 4096 || cfg->height > 4096) {
        fprintf(stderr, "width/height out of range\n");
        return false;
    }
    if (cfg->fps < 0 || cfg->fps > 120) { fprintf(stderr, "fps out of range\n"); return false; }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 2; }
    const std::string cmd = argv[1];

    if (cmd == "status") {
        fputs(aid::Camera::probeStatusJson().c_str(), stdout);
        return 0;
    }

    aid::CameraConfig cfg;
    int quality = 80;
    std::string err;

    if (cmd == "fps") {
        double seconds = 5.0;
        int optStart = 2;
        if (argc > 2 && argv[2][0] != '-') { seconds = atof(argv[2]); optStart = 3; }
        if (seconds < 1.0 || seconds > 600.0) { fprintf(stderr, "seconds must be 1..600\n"); return 2; }
        if (!parseOpts(argc, argv, optStart, &cfg, &quality)) return 2;

        aid::Camera cam;
        if (!cam.open(cfg, &err)) { fprintf(stderr, "open failed: %s\n", err.c_str()); return 1; }
        aid::CameraStats st;
        if (!cam.measureFps(seconds, &st, &err)) { fprintf(stderr, "fps failed: %s\n", err.c_str()); return 1; }
        printf("{\"width\": %u, \"height\": %u, \"frames\": %llu, \"seconds\": %.3f, \"fps\": %.2f}\n",
               st.width, st.height, (unsigned long long)st.frames, st.seconds, st.fps);
        return 0;
    }

    if (cmd == "snapshot") {
        if (argc < 3) { usage(argv[0]); return 2; }
        const std::string path = argv[2];
        if (!parseOpts(argc, argv, 3, &cfg, &quality)) return 2;

        aid::Camera cam;
        if (!cam.open(cfg, &err)) { fprintf(stderr, "open failed: %s\n", err.c_str()); return 1; }
        aid::SnapshotInfo info;
        if (!cam.snapshotJpeg(path, quality, &info, &err)) {
            fprintf(stderr, "snapshot failed: %s\n", err.c_str());
            return 1;
        }
        printf("{\"file\": \"%s\", \"bytes\": %u, \"width\": %u, \"height\": %u, \"encode_ms\": %.2f}\n",
               path.c_str(), info.bytes, info.width, info.height, info.encodeMs);
        return 0;
    }

    if (cmd == "controls") {
        if (!parseOpts(argc, argv, 2, &cfg, &quality)) return 2;
        aid::Camera cam;
        if (!cam.open(cfg, &err)) { fprintf(stderr, "open failed: %s\n", err.c_str()); return 1; }
        // Give the ISP a moment, then read the values back so we see what it really holds.
        usleep(1000 * 1000);
        printf("%s\n", cam.controlsJson().c_str());
        return 0;
    }

    usage(argv[0]);
    return 2;
}
