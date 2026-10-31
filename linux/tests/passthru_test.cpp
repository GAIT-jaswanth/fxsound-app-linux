// Drives AudioPassthru the way the FxSound GUI does, for end-to-end tests
// against a running PipeWire session.
//   passthru_test <preset.fac> <seconds> [target-node-name] [--switch-to name@sec] [--off-at sec]
#include "AudioPassthru.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

struct Cb : AudioPassthruCallback {
    std::atomic<int> calls{0};
    void onSoundDeviceChange(bool processing) override {
        calls++; printf("[cb] onSoundDeviceChange(processing=%d)\n", processing); fflush(stdout);
    }
};

static void dump(AudioPassthru& ap, const char* when) {
    printf("--- devices (%s), available=%d\n", when, ap.isPlaybackDeviceAvailable());
    for (auto& d : ap.getSoundDevices())
        printf("  %-28ls %-26ls real=%d dfx=%d default=%d target=%d ch=%d\n", d.deviceFriendlyName.c_str(),
               d.pwszID.c_str(), d.isRealDevice, d.isDFXDevice, d.isDefaultDevice, d.isTargetedRealPlaybackDevice,
               d.deviceNumChannel);
    fflush(stdout);
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s preset.fac seconds [target] [--switch-to name@sec] [--off-at sec] [--bypass]\n", argv[0]); return 2; }
    std::string preset = argv[1]; double secs = atof(argv[2]);
    std::string target = argc > 3 && argv[3][0] != '-' ? argv[3] : "";
    std::string switch_to; double switch_at = -1, off_at = -1; bool bypass = false;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--switch-to") && i + 1 < argc) { std::string a = argv[++i]; auto p = a.find('@'); switch_to = a.substr(0, p); switch_at = atof(a.c_str() + p + 1); }
        if (!strcmp(argv[i], "--off-at") && i + 1 < argc) off_at = atof(argv[++i]);
        if (!strcmp(argv[i], "--bypass")) bypass = true;
    }
    DfxDsp dsp;
    if (dsp.loadPreset(std::wstring(preset.begin(), preset.end())) != 0) { fprintf(stderr, "preset load failed\n"); return 1; }
    dsp.powerOn(!bypass);

    AudioPassthru ap; Cb cb;
    if (ap.init() != 0) { fprintf(stderr, "init failed\n"); return 1; }
    ap.setDspProcessingModule(&dsp);
    ap.registerCallback(&cb);
    dump(ap, "after init");
    for (auto& d : ap.getSoundDevices())
        if (d.isRealDevice && std::string(d.pwszID.begin(), d.pwszID.end()) == target) ap.setAsPlaybackDevice(d);

    auto t0 = std::chrono::steady_clock::now();
    bool on = true, switched = false;
    printf("READY\n"); fflush(stdout);
    for (;;) {
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (t >= secs) break;
        if (on && off_at >= 0 && t >= off_at) { on = false; dsp.powerOn(false); ap.restoreDefaultPlaybackDevice(); printf("[t=%.1f] power off\n", t); dump(ap, "power off"); }
        if (!switched && switch_at >= 0 && t >= switch_at) {
            switched = true;
            std::string cmd = "wpctl set-default $(pw-cli i " + switch_to + " | awk '/^[ \\t]*id:/{print $2; exit}') </dev/null >/dev/null 2>&1";
            int rc = system(cmd.c_str());
            printf("[t=%.1f] user switched system output to %s (rc=%d)\n", t, switch_to.c_str(), rc);
        }
        if (on) ap.processTimer();   // the GUI's 100 ms timer only runs while power is on
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    dump(ap, "end");
    printf("callbacks=%d\n", cb.calls.load());
    return 0;
}
