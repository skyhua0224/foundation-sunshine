#!/usr/bin/env python3
"""Fetch MSYS2 UCRT64 packages and their dependency closure into a prefix.

usage: msys2-sysroot.py DEST PACKAGE...   (names without the mingw-w64-ucrt-x86_64- prefix)

DEST/ucrt64 then holds the include/lib/share tree of the packages, laid out as /ucrt64 is inside MSYS2, so pkg-config files
(prefix=/ucrt64) resolve with PKG_CONFIG_SYSROOT_DIR=DEST. Package archives are verified against the SHA-256 of the repository
database that was fetched over https.
"""
import hashlib, io, os, re, subprocess, sys, tarfile, urllib.request

BASE = "https://repo.msys2.org/mingw/ucrt64/"
PFX = "mingw-w64-ucrt-x86_64-"
SKIP = {PFX + n for n in ("gcc-libs", "cc-libs", "libgcc", "libstdc++", "gcc", "binutils")}  # the cross compiler brings the compiler, its runtime libraries and binutils; crt, headers and winpthreads stay MSYS2 (same version as the Windows build)


def fetch(url):
    with urllib.request.urlopen(url) as r:
        return r.read()


def load_db():
    raw = fetch(BASE + "ucrt64.db")
    p = subprocess.run(["zstd", "-dc"] if raw[:4] == b"\x28\xb5\x2f\xfd" else ["gzip", "-dc"], input=raw, capture_output=True, check=True)
    db, provides = {}, {}
    with tarfile.open(fileobj=io.BytesIO(p.stdout)) as t:
        for m in t.getmembers():
            if not m.name.endswith("/desc"):
                continue
            fields, cur = {}, None
            for line in t.extractfile(m).read().decode().splitlines():
                if line.startswith("%") and line.endswith("%"):
                    cur = line.strip("%"); fields[cur] = []
                elif line and cur:
                    fields[cur].append(line)
            name = fields["NAME"][0]
            db[name] = fields
            for pr in fields.get("PROVIDES", []):
                provides[re.split("[<>=]", pr)[0]] = name
    return db, provides


def main():
    dest, wanted = sys.argv[1], sys.argv[2:]
    db, provides = load_db()
    todo = [PFX + w if not w.startswith("mingw-w64-") else w for w in wanted]
    seen, order = set(), []
    while todo:
        n = todo.pop()
        n = n if n in db else provides.get(n, n)
        if n in seen or n in SKIP:
            continue
        if n not in db:
            print(f"skip (not in ucrt64 repo): {n}", file=sys.stderr); continue
        seen.add(n); order.append(n)
        for d in db[n].get("DEPENDS", []):
            todo.append(re.split("[<>=]", d)[0])
    os.makedirs(dest, exist_ok=True)
    for n in sorted(order):
        f = db[n]["FILENAME"][0]
        data = fetch(BASE + f)
        if hashlib.sha256(data).hexdigest() != db[n]["SHA256SUM"][0]:
            sys.exit(f"checksum mismatch: {f}")
        subprocess.run(["tar", "--zstd", "-x", "-C", dest, "--exclude=.*"], input=data, check=True)
        print("installed", f)
    open(os.path.join(dest, "packages.txt"), "w").write("\n".join(db[n]["FILENAME"][0] for n in sorted(order)) + "\n")


main()
