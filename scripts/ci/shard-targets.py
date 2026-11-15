#!/usr/bin/env python3
"""Print the ninja outputs that shard INDEX of COUNT compiles.

usage: ninja -C build -t commands | shard-targets.py INDEX COUNT [WEIGHTS]

The input is the command list of the default target: only the object files named by `-o <path>.obj` are split, so targets
that are excluded from `all` (the Granite shader compiler, for one) are never compiled. (`ninja -n` is no use here: the dry run
of a fresh build directory stops at the pending CMake re-run.) Without WEIGHTS the outputs are dealt out in the order of
their path hash, which balances the count; with WEIGHTS (a .ninja_log of an earlier build) the longest-processing-time rule
puts the next-longest output on the least loaded shard. Every shard reads the same input, so the shards partition the set.
"""
import hashlib
import re
import sys


def main():
    index, count = int(sys.argv[1]), int(sys.argv[2])
    outputs = []
    for line in sys.stdin:
        m = re.search(r"\s-o\s+(\S+\.obj)(?:\s|$)", line)
        if m:
            outputs.append(m.group(1))
    outputs.sort(key=lambda p: hashlib.md5(p.encode()).hexdigest())
    weights = {}
    if len(sys.argv) > 3:
        with open(sys.argv[3]) as log:
            for line in log:
                parts = line.rstrip("\n").split("\t")
                if len(parts) == 5 and parts[0].isdigit():
                    weights[parts[3]] = int(parts[1]) - int(parts[0])
    if weights:
        known = sorted(weights.values())
        default = known[len(known) // 2] if known else 1
        outputs.sort(key=lambda p: (-weights.get(p, default), hashlib.md5(p.encode()).hexdigest()))
        load = [0] * count
        mine = []
        for p in outputs:
            bin_ = min(range(count), key=lambda b: (load[b], b))
            load[bin_] += weights.get(p, default)
            if bin_ == index:
                mine.append(p)
    else:
        mine = outputs[index::count]
    # Binary write: a text-mode print would end the lines with CRLF on Windows, which ninja reads as part of the name.
    sys.stdout.buffer.write(("\n".join(mine) + "\n").encode())


main()
