#!/usr/bin/env python3
"""Verify the dsp_prelude.h invariant.

The prelude includes all system headers, then does `#define long int`. If a
system header's declarations were first parsed AFTER that point, they'd be
declared with a 32-bit long and silently mismatch the real C library.

Method: preprocess each DSP source (as CMake does) and check that no line of
real code attributed to a system header (/usr/...) appears after the
preprocessor has entered the source file itself."""
import os, re, subprocess, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dsp = os.path.join(root, "dsp")
cm = open(os.path.join(root, "linux", "cmake_dsp_sources.cmake")).read()
srcs = [os.path.join(dsp, m) for m in re.findall(r"\$\{FX_ROOT\}/dsp/(\S+\.(?:c|cpp))", cm)]
srcs += [os.path.join(root, p) for p in (
    "linux/dsp_support/dsp_support.cpp", "audiopassthru/src/MTH/MthBuffer.cpp",
    "audiopassthru/src/MTH/MthUtil.cpp", "audiopassthru/src/MTH/Mthfreq.cpp",
    "audiopassthru/src/SLOUT/Slout.cpp")]
incs = ["-I" + os.path.join(root, "linux/compat")]
incs += ["-I" + os.path.join(root, d) for d in re.findall(r"\$\{FX_ROOT\}/(\S+)", cm.split("DFXDSP_INCLUDE_DIRS")[1])]
marker = re.compile(r'^# \d+ "([^"]+)"')
bad = 0
for src in srcs:
    cc = ["gcc", "-std=gnu11"] if src.endswith(".c") else ["g++", "-std=c++17", "-fpermissive"]
    cmd = cc + ["-E", "-w", "-DPT_NON_MFC", "-DDSPSOFT_TARGET", "-DPT_DSP_BUILD=PT_DSP_DFX",
                "-include", os.path.join(root, "linux/compat/dsp_prelude.h")] + incs + [src]
    out = subprocess.run(cmd, cwd=dsp, capture_output=True, text=True, errors="replace").stdout
    # GCC announces the source file once before processing the -include'd
    # prelude, so only start watching after the prelude has been entered.
    cur, seen_prelude, in_source, hits = None, False, False, []
    for line in out.splitlines():
        m = marker.match(line)
        if m:
            cur = m.group(1)
            if cur.endswith("dsp_prelude.h"):
                seen_prelude = True
            elif seen_prelude and os.path.abspath(os.path.join(dsp, cur)) == os.path.abspath(src):
                in_source = True
            continue
        if in_source and cur and cur.startswith("/usr/") and line.strip():
            hits.append(f"{cur}: {line.strip()[:80]}")
    if hits:
        bad += 1
        print(f"VIOLATION in {os.path.relpath(src, root)}:")
        for h in hits[:3]: print("   ", h)
print("dsp_prelude check:", "OK" if not bad else f"{bad} file(s) FAILED", f"({len(srcs)} files)")
sys.exit(1 if bad else 0)
