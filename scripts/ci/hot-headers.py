#!/usr/bin/env python3
"""Headers that a large share of the compile time depends on, and the check that a change touches one.

usage: ninja -C build -t deps | hot-headers.py generate SOURCE_ROOT WEIGHTS [THRESHOLD_PERCENT] > scripts/ci/hot-headers.txt
       changed-files-list | hot-headers.py check scripts/ci/hot-headers.txt
       hot-headers.py propose CURRENT MEASURED [CURRENT_WEIGHTS MEASURED_WEIGHTS] >> "$GITHUB_STEP_SUMMARY"

A change to a header that many translation units include misses the compiler cache for all of them. How much that costs depends on
how long those translation units take to compile, not on how many there are. `generate` adds up, per header of the repository's own
sources (third-party and generated headers excluded), the compile time of the object files whose dependency list names it (the
durations of WEIGHTS, a .ninja_log of a build: scripts/ci/shard-weights.ninja_log or a build's own build/.ninja_log), and keeps the
headers whose share of the whole compile time is at or above the threshold (default 30%). `check` reads changed paths on stdin and
prints the hot headers among them with their share (exit status 0 when there is at least one, 1 when there is none).
The file is a measurement of one build: the nightly build measures a fresh one (artifact ci-measurements, build_win), and the file is
regenerated from it when the include graph or the weights have moved. `propose` is that comparison for the nightly job summary: it
says whether the committed list is still current and, when it is not, shows what moved and the list to commit. It changes nothing.
"""
import collections
import os
import sys

# A header whose share moved by less than this many percentage points does not make the list stale (the share is one build's measurement).
SHARE_TOLERANCE = 5


def load_weights(path):
    weights = {}
    with open(path, encoding="utf-8") as log:
        for line in log:
            parts = line.rstrip("\n").split("\t")
            if len(parts) == 5 and parts[0].isdigit():
                weights[parts[3]] = int(parts[1]) - int(parts[0])
    return weights


def generate(root, weights_path, threshold):
    root = root.rstrip("/") + "/"
    weights = load_weights(weights_path)
    known = sorted(weights.values())
    default = known[len(known) // 2] if known else 1
    objects, total, time_of, current = 0, 0, collections.Counter(), None
    for line in sys.stdin:
        if line.startswith("    "):
            header = line.strip()
            if current and header.startswith(root) and "/third-party/" not in header and not header.endswith((".cpp", ".c")):
                time_of[header[len(root):]] += weights.get(current, default)
        elif line.strip():
            current = line.split(":")[0]
            if current.endswith(".obj"):
                objects += 1
                total += weights.get(current, default)
            else:
                current = None
    print(f"# headers whose object files make up at least {threshold:g}% of the compile time of {objects} object files (hot-headers.py generate)")
    for header, spent in sorted(time_of.items(), key=lambda item: (-item[1], item[0])):
        if 100 * spent >= threshold * total:
            print(f"{header}\t{round(100 * spent / total)}")


def check(path):
    hot = {}
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            if line.strip() and not line.startswith("#"):
                name, _, share = line.rstrip("\n").partition("\t")
                hot[name] = share
    hits = [f"{name} ({hot[name]}% of the compile time)" for name in (line.strip() for line in sys.stdin) if name in hot]
    print("\n".join(hits))
    return 0 if hits else 1


def load_list(path):
    hot = {}
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            if line.strip() and not line.startswith("#"):
                name, _, share = line.rstrip("\n").partition("\t")
                hot[name] = int(share)
    return hot


def weights_drift(current_path, measured_path):
    """One line: how much of the measured compile time the committed weights know. Unknown objects are split by the median."""
    current, measured = load_weights(current_path), load_weights(measured_path)
    total = sum(measured.values()) or 1
    unknown = {name: ms for name, ms in measured.items() if name not in current}
    gone = len(set(current) - set(measured))
    return (f"- shard weights: {len(measured)} object files measured, {len(current)} in the committed file; {len(unknown)} measured objects "
            f"({round(100 * sum(unknown.values()) / total)}% of the compile time) are not in it, {gone} of its objects no longer exist.")


def propose(current_path, measured_path, current_weights=None, measured_weights=None):
    """The nightly job summary (Markdown): is the committed list current, what moved, and the list to commit. Nothing is written."""
    current, measured = load_list(current_path), load_list(measured_path)
    moved = {name: (current.get(name), measured.get(name)) for name in sorted(set(current) | set(measured))
             if name not in current or name not in measured or abs(current[name] - measured[name]) >= SHARE_TOLERANCE}
    out = ["### hot-headers.txt"]
    if not moved:
        out.append(f"The committed list is current: the same {len(current)} headers, every share within {SHARE_TOLERANCE} points of this night's measurement.")
    else:
        out.append(f"The committed list has moved from this night's measurement ({len(current)} headers committed, {len(measured)} measured). "
                   f"Commit the measured file to refresh it; nothing was changed by this run.")
        out.append("")
        out.append("| header | committed | measured |")
        out.append("| --- | --- | --- |")
        for name, (old, new) in moved.items():
            out.append(f"| `{name}` | {'-' if old is None else f'{old}%'} | {'-' if new is None else f'{new}%'} |")
        out.append("")
        run, repo = os.environ.get("GITHUB_RUN_ID"), os.environ.get("GITHUB_REPOSITORY")
        if run and repo:
            out.append(f"`gh run download {run} -R {repo} -n ci-measurements -D measurements && cp measurements/hot-headers.txt scripts/ci/hot-headers.txt`")
            out.append("")
        out.append("<details><summary>Proposed scripts/ci/hot-headers.txt</summary>")
        out.append("")
        out.append("```")
        out.append(open(measured_path, encoding="utf-8").read().rstrip("\n"))
        out.append("```")
        out.append("</details>")
    if current_weights and measured_weights:
        out.append("")
        out.append(weights_drift(current_weights, measured_weights))
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 3 and sys.argv[1] == "generate":
        generate(sys.argv[2], sys.argv[3], float(sys.argv[4]) if len(sys.argv) > 4 else 30.0)
    elif len(sys.argv) > 2 and sys.argv[1] == "check":
        sys.exit(check(sys.argv[2]))
    elif len(sys.argv) > 3 and sys.argv[1] == "propose":
        sys.exit(propose(*sys.argv[2:6]))
    else:
        sys.exit(__doc__)
