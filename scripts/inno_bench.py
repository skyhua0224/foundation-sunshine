"""Inno Setup speed/size benchmark on the shipped payload (fork test only)."""
import json, os, re, subprocess, sys, time, glob, shutil, statistics

ISCC = r"C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
staging, work, repo = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(work, exist_ok=True)
tpl = open(os.path.join(repo, "cmake/packaging/sunshine.iss.in"), encoding="utf-8").read()
mat = 1 if os.path.exists(os.path.join(staging, "tools/mat-capture/sunshine-plugin-mat-capture.exe")) else 0
subs = {
    "CMAKE_PROJECT_NAME": "Sunshine", "CPACK_PACKAGE_VERSION": "2026.1114.173055",
    "CPACK_PACKAGE_VENDOR": "AlkaidLab", "CMAKE_PROJECT_HOMEPAGE_URL": "https://alkaidlab.com/sunshine",
    "INNO_STAGING_DIR": staging, "CMAKE_SOURCE_DIR": repo, "SUNSHINE_MAT_CAPTURE_AVAILABLE": str(mat),
}
base = "Compression=lzma2/ultra64\nSolidCompression=yes"
assert base in tpl
VARIANTS = [
    ("base ultra64 (today)", "Compression=lzma2/ultra64\nSolidCompression=yes"),
    ("ultra64 separate process", "Compression=lzma2/ultra64\nSolidCompression=yes\nLZMAUseSeparateProcess=yes"),
    ("ultra64 t4 block 64M", "Compression=lzma2/ultra64\nSolidCompression=yes\nLZMANumBlockThreads=4\nLZMABlockSize=65536"),
    ("ultra64 t4 block 32M", "Compression=lzma2/ultra64\nSolidCompression=yes\nLZMANumBlockThreads=4\nLZMABlockSize=32768"),
    ("ultra64 t4 block 16M", "Compression=lzma2/ultra64\nSolidCompression=yes\nLZMANumBlockThreads=4\nLZMABlockSize=16384"),
    ("ultra64 t4 block 64M separate", "Compression=lzma2/ultra64\nSolidCompression=yes\nLZMANumBlockThreads=4\nLZMABlockSize=65536\nLZMAUseSeparateProcess=yes"),
    ("ultra64 t4 block 32M separate", "Compression=lzma2/ultra64\nSolidCompression=yes\nLZMANumBlockThreads=4\nLZMABlockSize=32768\nLZMAUseSeparateProcess=yes"),
    ("ultra64 t2 block 64M separate", "Compression=lzma2/ultra64\nSolidCompression=yes\nLZMANumBlockThreads=2\nLZMABlockSize=65536\nLZMAUseSeparateProcess=yes"),
]
REPEATS = int(os.environ.get("REPEATS", "3"))
only = os.environ.get("ONLY")
if only:
    VARIANTS = [v for i, v in enumerate(VARIANTS) if str(i) in only.split(",")]
print("cpu_count", os.cpu_count(), flush=True)
results = {v[0]: [] for v in VARIANTS}
for rep in range(REPEATS):
    for idx, (name, lines) in enumerate(VARIANTS):
        out = os.path.join(work, "out")
        shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
        text = tpl.replace(base, lines)
        text = text.replace("OutputDir=@CPACK_PACKAGE_DIRECTORY@", "OutputDir=" + out)
        for k, v in subs.items():
            text = text.replace("@%s@" % k, v)
        iss = os.path.join(work, "bench.iss")
        open(iss, "w", encoding="utf-8-sig").write(text)
        t0 = time.perf_counter()
        p = subprocess.run([ISCC, iss], capture_output=True, text=True, encoding="utf-8", errors="replace")
        dt = time.perf_counter() - t0
        if p.returncode != 0:
            print(p.stdout[-3000:], p.stderr[-2000:]); sys.exit(1)
        exe = glob.glob(os.path.join(out, "*.exe"))[0]
        size = os.path.getsize(exe)
        results[name].append((dt, size))
        print(f"rep {rep+1} [{idx}] {name}: {dt:.1f} s, {size} bytes", flush=True)
base_name = VARIANTS[0][0]
bt = statistics.mean(r[0] for r in results[base_name]); bs = statistics.mean(r[1] for r in results[base_name])
lines = ["| variant | time s (3 runs) | mean s | size MB | size vs today | time vs today |", "|---|---|---|---|---|---|"]
for name, rs in results.items():
    mt = statistics.mean(r[0] for r in rs); ms = statistics.mean(r[1] for r in rs)
    lines.append(f"| {name} | {', '.join('%.1f' % r[0] for r in rs)} | {mt:.1f} | {ms/1e6:.2f} | {100*(ms/bs-1):+.2f}% | {100*(mt/bt-1):+.1f}% |")
print("\n".join(lines))
with open(os.environ.get("GITHUB_STEP_SUMMARY", os.devnull), "a") as f:
    f.write("\n".join(lines) + "\n")
