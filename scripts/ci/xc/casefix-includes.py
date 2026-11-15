#!/usr/bin/env python3
"""Windows file systems ignore case, Linux ones do not: the sources say <Windows.h>, <Unknwn.h>, <Shlwapi.h>, mingw-w64 ships windows.h,
unknwn.h, shlwapi.h. For every system include of the sources that no search directory satisfies, link the name to the header of the
same name ignoring case, in DEST (put DEST on the include path with -isystem).

usage: casefix-includes.py DEST SOURCE_DIR... -- INCLUDE_DIR...
"""
import os, re, sys

args = sys.argv[1:]
dest = args[0]
sep = args.index("--")
srcs, incs = args[1:sep], args[sep + 1:]
pat = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
wanted = set()
for root in srcs:
    for d, _, files in os.walk(root):
        if "/.git" in d:
            continue
        for f in files:
            if f.endswith((".h", ".hpp", ".cpp", ".c", ".cc", ".hh", ".inl", ".ipp")):
                try:
                    wanted.update(pat.findall(open(os.path.join(d, f), errors="replace").read()))
                except OSError:
                    pass
index = {}
for inc in incs:
    for d, _, files in os.walk(inc):
        rel = os.path.relpath(d, inc)
        for f in files:
            p = os.path.normpath(os.path.join(rel, f))
            index.setdefault(p.lower(), os.path.join(d, f))
os.makedirs(dest, exist_ok=True)
made = 0
for w in sorted(wanted):
    if any(os.path.exists(os.path.join(i, w)) for i in incs):
        continue
    hit = index.get(os.path.normpath(w).lower())
    if hit:
        target = os.path.join(dest, w)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        if not os.path.lexists(target):
            os.symlink(hit, target); made += 1
print(f"casefix: {made} links in {dest}")
