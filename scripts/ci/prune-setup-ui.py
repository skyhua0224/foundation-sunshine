#!/usr/bin/env python3
"""Remove assets the installer's setup UI never uses before Tauri embeds them.

The setup UI pages are foundation-ui's MInstaller root. Its stylesheet is the whole
foundation-ui stylesheet, so it also carries the clock components and their four
fonts; the page has no clock (its built JavaScript mentions no clock class), and a
clock rule or font that is never applied cannot change the window. The clock rules
and the @font-face blocks that point at those files are removed from the emitted
CSS, the font files are deleted, and source maps (none in the release build) are
removed. The script refuses to delete a file that the remaining CSS, JavaScript or
HTML still names, and writes a JSON report with the bytes of every removal.

usage: prune-setup-ui.py --dist dist-installer [--report report.json]

Exit codes: 0 = pruned, 2 = a refused removal or a missing input.

The embedded-directory shape is the one Tauri documents for frontendDist
(fs-tauri-config-20261122).
"""

import argparse
import json
import pathlib
import re
import sys

CLOCK_FONT = re.compile(r"^clock-[A-Za-z0-9_-]+\.woff2$")


def _matching_brace(css, start):
    depth = 0
    for index in range(start, len(css)):
        if css[index] == "{":
            depth += 1
        elif css[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    raise ValueError("unbalanced CSS block")


def _strip_clock(css):
    """Drop rules whose selector is a clock class and clock @font-face blocks, recursively through at-rules."""
    out, index, length = [], 0, len(css)
    while index < length:
        brace = css.find("{", index)
        if brace < 0:
            out.append(css[index:])
            break
        prelude = css[index:brace]
        end = _matching_brace(css, brace)
        body = css[brace + 1:end]
        lowered = prelude.lower()
        drop = False
        if lowered.lstrip().startswith("@font-face"):
            drop = "clock-" in body.lower() or re.search(r"font-family\s*:\s*clock", body, re.I) is not None
        elif lowered.lstrip().startswith(("@media", "@supports", "@layer", "@container", "@scope")):
            body = _strip_clock(body)
            drop = not body.strip()
        elif "clock" in lowered:
            drop = True
        if not drop:
            out.append(prelude + "{" + body + "}")
        index = end + 1
    return "".join(out)


def _referenced(root):
    """Every file name the remaining CSS, JavaScript and HTML mentions."""
    names = set()
    for path in root.rglob("*"):
        if path.suffix.lower() not in (".css", ".js", ".html", ".json"):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        names.update(re.findall(r"[A-Za-z0-9._-]+\.[A-Za-z0-9]{2,5}", text))
    return names


def prune(dist, report_path=None):
    dist = pathlib.Path(dist)
    if not dist.is_dir():
        raise SystemExit(f"prune-setup-ui: {dist} is not a directory")
    removed, rewritten = [], []

    for css_path in sorted(dist.rglob("*.css")):
        before = css_path.read_text(encoding="utf-8")
        after = _strip_clock(before)
        if after != before:
            css_path.write_text(after, encoding="utf-8")
            rewritten.append({"file": str(css_path.relative_to(dist)), "before": len(before.encode()), "after": len(after.encode())})

    referenced = _referenced(dist)
    clock_fonts = [p for p in dist.rglob("*") if p.is_file() and CLOCK_FONT.match(p.name)]
    for path in clock_fonts:
        if path.name in referenced:
            raise SystemExit(f"prune-setup-ui: refusing to remove {path.relative_to(dist)}: still referenced")
    for path in sorted(dist.rglob("*.map")):
        if path.name in referenced:
            raise SystemExit(f"prune-setup-ui: refusing to remove {path.relative_to(dist)}: still referenced")
        removed.append({"file": str(path.relative_to(dist)), "bytes": path.stat().st_size})
        path.unlink()
    for path in clock_fonts:
        removed.append({"file": str(path.relative_to(dist)), "bytes": path.stat().st_size})
        path.unlink()

    for path in sorted(dist.rglob("*")):
        if path.is_file() and path.suffix.lower() in (".css", ".js", ".html"):
            text = path.read_text(encoding="utf-8", errors="replace")
            for name in re.findall(r"(?:src|href)=\"\./([^\"]+)\"", text):
                if not (path.parent / name).exists():
                    raise SystemExit(f"prune-setup-ui: {path.relative_to(dist)} names a missing file: {name}")

    result = {
        "removed": removed,
        "removed_bytes": sum(entry["bytes"] for entry in removed),
        "rewritten": rewritten,
        "rewritten_bytes": sum(entry["before"] - entry["after"] for entry in rewritten),
        "locales_removed": 0,
        "debug_info_removed": 0,
    }
    if report_path:
        pathlib.Path(report_path).write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    for entry in removed:
        print(f"removed {entry['file']}: {entry['bytes']:,} bytes")
    for entry in rewritten:
        print(f"rewrote {entry['file']}: {entry['before'] - entry['after']:,} bytes")
    print(f"setup UI assets: {result['removed_bytes'] + result['rewritten_bytes']:,} bytes reclaimed")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dist", required=True, help="the dist-installer directory that Tauri embeds")
    parser.add_argument("--report", help="write the per-removal report here")
    args = parser.parse_args(argv)
    prune(args.dist, args.report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
