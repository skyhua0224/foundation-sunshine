#!/usr/bin/env python3
"""Same problem as casefix-includes.py, for libraries: the link lines say -lWindowsapp, -lD3DCompiler, -lPowrProf, the import libraries are
libwindowsapp.a, libd3dcompiler.a, libpowrprof.a. Link every -lNAME of BUILD_NINJA that no library directory satisfies to the library of the same name
ignoring case, in DEST (put DEST on the library path with -L).

usage: casefix-libs.py DEST BUILD_NINJA LIB_DIR...
"""
import os, re, sys

dest, ninja, libdirs = sys.argv[1], sys.argv[2], sys.argv[3:]
names = set(re.findall(r"(?<=\s)-l([A-Za-z0-9_+.-]+)", open(ninja, errors="replace").read()))
index = {}
for d in libdirs:
    for f in os.listdir(d):
        index.setdefault(f.lower(), os.path.join(d, f))
os.makedirs(dest, exist_ok=True)
made = 0
for n in sorted(names):
    for ext in (".a", ".dll.a"):
        f = f"lib{n}{ext}"
        if any(os.path.exists(os.path.join(d, f)) for d in libdirs):
            break
        hit = index.get(f.lower())
        if hit:
            t = os.path.join(dest, f)
            if not os.path.lexists(t):
                os.symlink(hit, t); made += 1
            break
print(f"casefix-libs: {made} links in {dest}")
