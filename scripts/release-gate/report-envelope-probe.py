#!/usr/bin/env python3
"""Envelope probe for the diagnostics endpoint (release gate).

Sends two empty event batches to <endpoint>/v1/events:
  1. an envelope with the version string of the build under test -> must be accepted (HTTP 200)
  2. the same envelope with an empty version                      -> must be refused (HTTP 400 bad_envelope)
The second one is the negative control: a server that accepts everything would make the first check
meaningless. The 2026-11-14 prerelease failed (1) because its version "v2026.1114.173055.<name>" was
sanitised to an empty string by the host.

Only loopback endpoints are allowed, so the gate can never write test data into a public service.

usage: report-envelope-probe.py --endpoint http://127.0.0.1:18787 --version "v2026.1115.12345.<name>" [--out probe.json]
"""
import argparse, json, sys, urllib.error, urllib.parse, urllib.request, uuid

ap = argparse.ArgumentParser()
ap.add_argument('--endpoint', required=True)
ap.add_argument('--version', required=True)
ap.add_argument('--out')
a = ap.parse_args()
host = urllib.parse.urlparse(a.endpoint).hostname
if host not in ('127.0.0.1', 'localhost', '::1'):
    sys.exit('refusing a non-loopback endpoint: ' + a.endpoint)


def post(version):
    body = {'v': 1, 'install_id': str(uuid.uuid4()), 'session_id': str(uuid.uuid4()), 'product': 'host',
            'version': version, 'channel': 'dev', 'os': 'windows', 'os_version': '10.0.26100', 'arch': 'x64', 'locale': 'zh-CN', 'events': []}
    req = urllib.request.Request(a.endpoint.rstrip('/') + '/v1/events', data=json.dumps(body).encode(), headers={'content-type': 'application/json'}, method='POST')
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


res = []
code, txt = post(a.version)
res.append({'id': 'report.envelope-accepted', 'status': 'pass' if code == 200 else 'fail', 'detail': f'HTTP {code} {txt[:120]}'})
code, txt = post('')
res.append({'id': 'report.negative-control', 'status': 'pass' if code == 400 else 'fail', 'detail': f'HTTP {code} {txt[:120]}'})
print(json.dumps(res, ensure_ascii=False, indent=2))
if a.out:
    open(a.out, 'w', encoding='utf-8').write(json.dumps(res, ensure_ascii=False, indent=2))
sys.exit(1 if any(r['status'] == 'fail' for r in res) else 0)
