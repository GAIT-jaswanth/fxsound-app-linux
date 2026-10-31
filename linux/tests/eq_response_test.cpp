// Checks that the DSP's measured frequency response follows the loaded
// preset's graphic-EQ curve (with the other effects switched off).
#include "DfxDsp.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static double toneGainDb(DfxDsp& dsp, double freq)
{
    const int sr = 48000, N = sr, B = 480;
    std::vector<float> in(N * 2), out(N * 2);
    for (int i = 0; i < N; i++) in[2*i] = in[2*i+1] = (float)(0.05 * sin(2 * M_PI * freq * i / sr));
    for (int off = 0; off < N; off += B)
        dsp.processAudio((short*)&in[off * 2], (short*)&out[off * 2], B, 0);
    double ri = 0, ro = 0;
    for (int i = N; i < 2 * N; i++) { ri += in[i] * (double)in[i]; ro += out[i] * (double)out[i]; }  // 2nd half: settled
    return 10 * log10(ro / ri);
}

int main(int argc, char** argv)
{
    const char* preset = argc > 1 ? argv[1] : "bin/x64/Factsoft/3.fac";
    DfxDsp dsp;
    std::wstring p(preset, preset + strlen(preset));
    if (dsp.loadPreset(p) != 0) { printf("cannot load %s\n", preset); return 1; }
    dsp.setSignalFormat(32, 2, 48000, 32);
    for (int e = 0; e < DfxDsp::NumEffects; e++) dsp.setEffectValue((DfxDsp::Effect)e, 0.0f);
    dsp.powerOn(true);

    // Gain at each band centre, relative to the 1 kHz band.
    int n = dsp.getNumEqBands();
    std::vector<double> meas(n), want(n);
    int ref = 0;
    for (int b = 0; b < n; b++) if (fabs(dsp.getEqBandFrequency(b) - 1000) < 1) ref = b;
    double mref = toneGainDb(dsp, dsp.getEqBandFrequency(ref));
    double maxerr = 0, sxy = 0, sxx = 0, syy = 0, mx = 0, my = 0;
    printf("%8s %9s %9s\n", "Hz", "preset", "measured");
    for (int b = 0; b < n; b++) {
        double f = dsp.getEqBandFrequency(b);
        if (f > 20000) continue;
        want[b] = dsp.getEqBandBoostCut(b) - dsp.getEqBandBoostCut(ref);
        meas[b] = toneGainDb(dsp, f) - mref;
        printf("%8.0f %+8.2f %+8.2f\n", f, want[b], meas[b]);
        mx += want[b]; my += meas[b];
    }
    mx /= n; my /= n;
    for (int b = 0; b < n; b++) {
        sxy += (want[b]-mx)*(meas[b]-my); sxx += (want[b]-mx)*(want[b]-mx); syy += (meas[b]-my)*(meas[b]-my);
        maxerr = fmax(maxerr, fabs(want[b] - meas[b]));
    }
    double corr = sxx > 0 && syy > 0 ? sxy / sqrt(sxx * syy) : 1.0;
    printf("correlation(preset curve, measured) = %.3f   max deviation = %.2f dB\n", corr, maxerr);
    return 0;
}
