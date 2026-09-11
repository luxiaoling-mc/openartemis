#!/usr/bin/env python3
"""Pull the script/Lua/config subset out of a PFS archive (no assets).

Auditing a game only needs its scripts, Lua framework and tables; the images,
sounds and movies are gigabytes. This lists the archive index, keeps the
interesting extensions and fetches each entry with `opfs get`.

    python tools/pfs_subset.py <opfs.exe> <archive> <outdir>
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

KEEP = {".ast", ".asb", ".iet", ".lua", ".tbl", ".ini", ".ipt", ".rft", ".sli"}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("opfs", type=Path)
    ap.add_argument("archive", type=Path)
    ap.add_argument("outdir", type=Path)
    ap.add_argument("--ext", default=",".join(sorted(KEEP)), help="extensions to keep")
    args = ap.parse_args()

    keep = {"." + e.strip().lstrip(".").lower() for e in args.ext.split(",") if e.strip()}
    listing = subprocess.run([str(args.opfs), "find", str(args.archive), ""],
                             capture_output=True, text=True, encoding="utf-8",
                             errors="replace")
    if listing.returncode != 0:
        print(listing.stderr, file=sys.stderr)
        return 1

    names = []
    for raw in listing.stdout.splitlines():
        line = raw.strip()
        if not line:
            continue
        # `opfs find` prints "<size>  <path>"; keep just the path.
        parts = line.split(None, 1)
        names.append(parts[1].strip() if len(parts) == 2 and parts[0].isdigit() else line)
    wanted = [n for n in names if Path(n).suffix.lower() in keep]
    print(f"{len(names)} entries, {len(wanted)} to fetch", flush=True)

    ok = failed = 0
    total = 0
    for name in wanted:
        rel = name.replace("\\", "/").lstrip("/")
        out = args.outdir / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        r = subprocess.run([str(args.opfs), "dump", str(args.archive), name, str(out)],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
        if r.returncode == 0 and out.exists() and out.stat().st_size:
            ok += 1
            total += out.stat().st_size
        else:
            failed += 1
            print(f"!! {name}: {r.stderr.strip()[:120]}", file=sys.stderr)
    print(f"{ok} fetched ({total/1024:.0f} KB), {failed} failed -> {args.outdir}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
