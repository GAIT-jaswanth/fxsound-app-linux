// Offline reference: processes raw interleaved stereo f32 (stdin -> stdout)
// through DfxDsp with a preset, in fixed-size blocks.
//   dsp_process_raw <preset.fac> <rate> [block] [--bypass] [--bands N] [--bands-after-preset] [--gui-sequence]
// --bands N mirrors the GUI's EQ band setting. The GUI calls setNumBands()
// before loading a preset at startup; --bands-after-preset reverses that order.
// --gui-sequence replays FxController's startup defaults and the calls setPreset()
// makes after loadPreset().
#include "DfxDsp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s preset.fac rate [block] [--bypass] [--bands N] [--bands-after-preset]\n", argv[0]); return 2; }
    std::string p = argv[1];
    int rate = atoi(argv[2]);
    int block = argc > 3 && argv[3][0] != '-' ? atoi(argv[3]) : 480;
    bool bypass = false, bands_after = false, gui_sequence = false;
    int bands = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--bypass")) bypass = true;
        else if (!strcmp(argv[i], "--bands") && i + 1 < argc) bands = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bands-after-preset")) bands_after = true;
        else if (!strcmp(argv[i], "--gui-sequence")) gui_sequence = true;
    }
    DfxDsp dsp;
    if (bands > 0 && !bands_after) dsp.setNumBands(bands);
    if (gui_sequence) {
        // FxController::initConfig() defaults, applied before the first preset load
        dsp.setVolumeLeveling(0.0f);
        dsp.setBalance(0.0f);
        dsp.setFilterQ(1.0f);
        dsp.setMasterGain(0.0f);
    }
    if (dsp.loadPreset(std::wstring(p.begin(), p.end())) != 0) { fprintf(stderr, "preset load failed\n"); return 1; }
    if (bands > 0 && bands_after) dsp.setNumBands(bands);
    if (gui_sequence) {
        // Same as fxsound/Source/GUI/FxController.cpp, setPreset()
        for (int e = 0; e < DfxDsp::NumEffects; e++)
            dsp.setEffectValue((DfxDsp::Effect)e, dsp.getEffectValue((DfxDsp::Effect)e) * 10);
        for (int b = 0; b < dsp.getNumEqBands(); b++) {
            dsp.setEqBandFrequency(b, dsp.getEqBandFrequency(b));
            dsp.setEqBandBoostCut(b, dsp.getEqBandBoostCut(b));
        }
    }
    dsp.powerOn(!bypass);
    dsp.setSignalFormat(32, 2, rate, 32);
    std::vector<float> buf(block * 2);
    size_t n;
    while ((n = fread(buf.data(), 8, block, stdin)) > 0) {
        dsp.processAudio((short*)buf.data(), (short*)buf.data(), (int)n, 0);
        fwrite(buf.data(), 8, n, stdout);
    }
    return 0;
}
