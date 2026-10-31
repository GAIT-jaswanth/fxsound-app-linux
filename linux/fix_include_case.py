#!/usr/bin/env python3
"""Rewrite #include "x.h" lines so their case matches the real file on disk.
Windows is case-insensitive; Linux is not. Safe to run repeatedly."""
import os, re, sys
roots = sys.argv[1:] or ["dsp", "audiopassthru", "fxsound/Source", "linux"]
headers = {}
for r in roots:
    for d, _, fs in os.walk(r):
        for f in fs:
            if f.lower().endswith((".h", ".hpp")):
                headers.setdefault(f.lower(), set()).add(f)
pat = re.compile(r'(#\s*include\s*")([^"]+)(")')
changed = 0
for r in roots:
    for d, _, fs in os.walk(r):
        for f in fs:
            if not f.lower().endswith((".c", ".cpp", ".h", ".hpp")): continue
            p = os.path.join(d, f)
            src = open(p, encoding="latin-1", newline="").read()
            def fix(m):
                inc = m.group(2).replace("\\", "/")
                head, base = os.path.split(inc)
                cands = headers.get(base.lower())
                if cands and base not in cands and len(cands) == 1:
                    base = next(iter(cands))
                return m.group(1) + (head + "/" if head else "") + base + m.group(3)
            new = pat.sub(fix, src)
            if new != src:
                open(p, "w", encoding="latin-1", newline="").write(new); changed += 1
print(f"updated {changed} files")
