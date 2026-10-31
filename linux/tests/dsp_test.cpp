#include "DfxDsp.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <vector>
// Process a 100 Hz + 5 kHz tone in 16-bit or float and report RMS gain.
static void run(DfxDsp& dsp, int bps, bool on) {
    const int N = 48000, B = 480;
    printf("  setSignalFormat(%d) -> %d\n", bps, dsp.setSignalFormat(bps, 2, 48000, bps));
    dsp.powerOn(on);
    std::vector<float> fin(N*2), fout(N*2); std::vector<short> sin_(N*2), sout(N*2);
    for (int i = 0; i < N; i++) { double v = 0.25*sin(2*M_PI*100*i/48000.0) + 0.1*sin(2*M_PI*5000*i/48000.0);
        fin[2*i]=fin[2*i+1]=(float)v; sin_[2*i]=sin_[2*i+1]=(short)(v*32767); }
    int bad = 0;
    for (int off = 0; off < N; off += B) {
        int r = bps == 32 ? dsp.processAudio((short*)&fin[off*2], (short*)&fout[off*2], B, 0)
                          : dsp.processAudio(&sin_[off*2], &sout[off*2], B, 0);
        bad += r != 0;
    }
    double ri=0, ro=0; int nan=0;
    for (int i = N; i < 2*N; i++) {
        double a = bps==32 ? fin[i] : sin_[i]/32767.0, b = bps==32 ? fout[i] : sout[i]/32767.0;
        if (std::isnan(b)) nan++; ri += a*a; ro += b*b; }
    printf("  bps=%d power=%s fail_calls=%d nan=%d  RMS in=%.4f out=%.4f (%.2f dB)\n", bps, on?"on ":"off", bad, nan,
        sqrt(ri/N), sqrt(ro/N), 10*log10((ro+1e-30)/ri));
}
int main(int argc, char** argv) {
    DfxDsp dsp;
    std::wstring p = L"bin/x64/Factsoft/" + std::wstring(argv[1], argv[1]+strlen(argv[1]));
    printf("load %s -> %d\n", argv[1], dsp.loadPreset(p));
    run(dsp, 32, true); run(dsp, 32, false); run(dsp, 16, true); run(dsp, 16, false);
}
