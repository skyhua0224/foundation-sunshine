"""Compare the committed template (master) with the PR template on the shipped payload (fork test only)."""
import glob, os, shutil, statistics, subprocess, sys, time
ISCC = r"C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
staging, work, repo = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(work, exist_ok=True)
mat = 1 if os.path.exists(os.path.join(staging, "tools/mat-capture/sunshine-plugin-mat-capture.exe")) else 0
subs = {"CMAKE_PROJECT_NAME": "Sunshine", "CPACK_PACKAGE_VERSION": "2026.1114.173055", "CPACK_PACKAGE_VENDOR": "AlkaidLab",
        "CMAKE_PROJECT_HOMEPAGE_URL": "https://alkaidlab.com/sunshine", "INNO_STAGING_DIR": staging, "CMAKE_SOURCE_DIR": repo,
        "SUNSHINE_MAT_CAPTURE_AVAILABLE": str(mat)}
variants = [("master template", open(os.path.join(repo, "master.iss.in"), encoding="utf-8").read()),
            ("PR template", open(os.path.join(repo, "cmake/packaging/sunshine.iss.in"), encoding="utf-8").read())]
res = {n: [] for n, _ in variants}
for rep in range(3):
    for name, tpl in variants:
        out = os.path.join(work, "out"); shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
        text = tpl.replace("OutputDir=@CPACK_PACKAGE_DIRECTORY@", "OutputDir=" + out)
        for k, v in subs.items(): text = text.replace("@%s@" % k, v)
        iss = os.path.join(work, "v.iss"); open(iss, "w", encoding="utf-8-sig").write(text)
        t0 = time.perf_counter()
        p = subprocess.run([ISCC, iss], capture_output=True, text=True, encoding="utf-8", errors="replace")
        dt = time.perf_counter() - t0
        if p.returncode: print(p.stdout[-2000:]); sys.exit(1)
        size = os.path.getsize(glob.glob(os.path.join(out, "*.exe"))[0]); res[name].append((dt, size))
        print(f"rep {rep+1} {name}: {dt:.1f} s, {size} bytes", flush=True)
bt = statistics.mean(r[0] for r in res["master template"]); bs = statistics.mean(r[1] for r in res["master template"])
rows = ["| template | time s (3 runs) | mean s | size MB | size | time |", "|---|---|---|---|---|---|"]
for n, rs in res.items():
    mt = statistics.mean(r[0] for r in rs); ms = statistics.mean(r[1] for r in rs)
    rows.append(f"| {n} | {', '.join('%.1f' % r[0] for r in rs)} | {mt:.1f} | {ms/1e6:.2f} | {100*(ms/bs-1):+.2f}% | {100*(mt/bt-1):+.1f}% |")
print("\n".join(rows))
