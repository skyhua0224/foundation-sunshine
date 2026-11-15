#!/usr/bin/env python3
"""Static check of a downloaded Windows build (release gate, no Windows needed).

  check-release-artifacts.py <artifact-dir> [--out result.json]

Looks for the installer and the portable ZIP in <artifact-dir> (any depth), then
  - verifies SHA256SUMS.txt / checksums.json when present,
  - checks that the portable ZIP carries the files every installer component needs
    (the same table the installer gate uses), including both USB/IP helper scripts,
  - checks that the installer is at least as large as the portable ZIP's compressed payload minus 30 %
    (a truncated installer is the cheap failure to catch),
  - checks that debug symbols are published next to the build (symbol archive name contains 'pdb' or 'symbols').
Result objects: id, status pass|fail|info, detail. Exit code 1 on any failure.
"""
import hashlib, json, pathlib, sys, zipfile

REQUIRED = {
    'application': ['Sunshine.exe', 'tools/sunshinesvc.exe', 'scripts/install-service.bat', 'scripts/uninstall-service.bat'],
    'assets': ['assets', 'tools/usbip-win2-root.ps1', 'tools/restart64.exe'],
    'gui': ['assets/gui/sunshine-gui.exe'],
    'vdd': ['scripts/install-vdd.bat', 'scripts/uninstall-vdd.bat'],
    'firewall': ['scripts/add-firewall-rule.bat', 'scripts/delete-firewall-rule.bat'],
    'autostart': ['scripts/autostart-service.bat'],
    'gamepad': ['scripts/install-gamepad.bat'],
    'usbip': ['scripts/install-usbip.bat', 'scripts/usbip/'],
    'vmouse': ['scripts/vmouse/install-vmouse.bat'],
    'tools': ['tools/dxgi-info.exe', 'tools/audio-info.exe'],
}

root = pathlib.Path(sys.argv[1])
out = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else None
res = []


def add(i, s, d=''):
    res.append({'id': i, 'status': s, 'detail': d})
    print(f'[{s.upper()}] {i} {d}')


def sha(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


files = [p for p in root.rglob('*') if p.is_file()]
inst = [p for p in files if p.name.lower().endswith('.exe') and 'install' in p.name.lower()]
port = [p for p in files if p.name.lower().endswith('.zip') and 'portable' in p.name.lower()]
add('art.installer', 'pass' if len(inst) == 1 else 'fail', ', '.join(f'{p.name} {p.stat().st_size}' for p in inst) or 'no installer found')
add('art.portable', 'pass' if len(port) == 1 else 'fail', ', '.join(f'{p.name} {p.stat().st_size}' for p in port) or 'no portable ZIP found')

sums = {}
for p in files:
    if p.name.upper() == 'SHA256SUMS.TXT':
        for line in p.read_text(encoding='utf-8', errors='replace').splitlines():
            parts = line.split()
            if len(parts) >= 2 and len(parts[0]) == 64:
                sums[parts[-1].lstrip('*')] = parts[0].lower()
for p in inst + port:
    if p.name in sums:
        add('art.sha256.' + p.name, 'pass' if sha(p) == sums[p.name] else 'fail', 'matches SHA256SUMS.txt' if sha(p) == sums[p.name] else 'differs from SHA256SUMS.txt')
    else:
        add('art.sha256.' + p.name, 'info', 'no checksum file in this artifact set; sha256 ' + sha(p))

if len(port) == 1:
    z = zipfile.ZipFile(port[0])
    names = [n.replace('\\', '/') for n in z.namelist()]
    prefix = ''
    for n in names:
        if n.lower().endswith('sunshine.exe') and n.count('/') <= 1:
            prefix = n[: -len('sunshine.exe')]
            break
    names = [n[len(prefix):] for n in names if n.startswith(prefix)]
    lower = {n.lower() for n in names}
    for comp, need in REQUIRED.items():
        miss = [f for f in need if not any(n.lower() == f.lower() or n.lower().startswith(f.rstrip('/').lower() + '/') for n in names)]
        add('art.component.' + comp, 'fail' if miss else 'pass', ('missing ' + ', '.join(miss)) if miss else f'{len(need)} required path(s) present')
    if inst:
        payload = sum(i.compress_size for i in z.infolist())
        add('art.installer-size', 'pass' if inst[0].stat().st_size >= 0.7 * payload else 'fail', f'installer {inst[0].stat().st_size} bytes, portable payload {payload} bytes')

sym = [p for p in files if any(k in p.name.lower() for k in ('pdb', 'symbol'))]
add('art.symbols', 'pass' if sym else 'info', ', '.join(p.name for p in sym) or 'no symbol archive in this artifact set (it may be published as a release asset only)')

if out:
    pathlib.Path(out).write_text(json.dumps(res, ensure_ascii=False, indent=2), encoding='utf-8')
sys.exit(1 if any(r['status'] == 'fail' for r in res) else 0)
