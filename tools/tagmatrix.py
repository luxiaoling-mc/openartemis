#!/usr/bin/env python3
"""Cross-game view of the engine tag surface.

`tagaudit.py` answers "does *this* game use the tag?". Several games together
answer the question that actually decides a deletion: "does *any* game we support
use it?".

Usage::

    python tools/tagmatrix.py --src src/core GAME=path [GAME=path ...]
    python tools/tagmatrix.py --src src/core fpm=/g/fpm/gt slny=/g/slny/sss
    python tools/tagmatrix.py --src src/core fpm=/g/fpm/gt --unused   # all-games-unused only
"""

from __future__ import annotations

import argparse
import collections
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tagaudit  # noqa: E402

MARK = {
    "used": "S",            # produced by a script (.ast/.iet)
    "shadowed": "L",        # Lua tags.X consumes it (script path) ...
    "shadowed?": "l",
    "engine-produced": "E", # ... but the engine enqueues it itself
    "game-only": "-",
    "ENGINE-ONLY": ".",
    "-": " ",
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--src", type=Path, default=Path("src/core"))
    ap.add_argument("games", nargs="+", metavar="NAME=PROJECT_ROOT")
    ap.add_argument("--asb-iet", help="NAME=DIR pairs for decoded ASB (repeatable)", action="append")
    ap.add_argument("--tag-docs", type=Path,
                    help="upstream tag reference directory (one doc per tag); engine tags "
                         "without a doc are flagged NOT-UPSTREAM")
    ap.add_argument("--unused", action="store_true", help="only tags unused by every game")
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()

    iet = {}
    for spec in args.asb_iet or []:
        name, _, path = spec.partition("=")
        iet[name] = Path(path) if path else None

    upstream = tagaudit.upstream_tags(args.tag_docs) if args.tag_docs else None
    engine, engine_emits = tagaudit.engine_surface(args.src)
    games: list[str] = []
    reports = {}
    for spec in args.games:
        name, _, path = spec.partition("=")
        root = Path(path)
        (lua_defs, lua_emit, lua_mentions, bracket, sites, ast,
         detail) = tagaudit.game_surface(root, iet.get(name))
        games.append(name)
        reports[name] = {
            "engine": engine,
            "engine_emit": engine_emits,
            "lua_def": lua_defs,
            "lua_emit": lua_emit,
            "mentions": lua_mentions,
            "bracket": bracket,
            "sites": sites,
            "ast": ast,
            "detail": detail,
        }

    rows = []
    for tag in sorted(set(engine) | set(engine_emits)):
        per = {}
        for name in games:
            r = reports[name]
            ast_rows = r["ast"].total.get(tag, 0)
            lua_tag = r["detail"]["lua_tag"].get(tag, 0)
            iet_rows = r["detail"]["iet"].get(tag, 0)
            lua_str = r["detail"]["lua_str"].get(tag, 0)
            emitted = bool(ast_rows or r["bracket"].get(tag, 0) or r["mentions"].get(tag))
            per[name] = {
                "verdict": tagaudit.verdict_for(
                    tag, bool(r["engine"].get(tag)), bool(r["engine_emit"].get(tag)),
                    bool(r["lua_def"].get(tag)), emitted,
                ),
                "ast": ast_rows,
                "lua_tag": lua_tag,
                "iet": iet_rows,
                "lua_str": lua_str,
                "mention": len(r["mentions"].get(tag, [])),
                "lua_def": bool(r["lua_def"].get(tag)),
                "enqueue": bool(r["lua_emit"].get(tag)),
                "engine_emit": bool(r["engine_emit"].get(tag)),
            }
        strong = [n for n in games if per[n]["ast"] or per[n]["lua_tag"] or per[n]["iet"]]
        weak = [n for n in games if not (per[n]["ast"] or per[n]["lua_tag"] or per[n]["iet"])
                and (per[n]["lua_str"] or per[n]["mention"])]
        enqueued_by = [n for n in games if per[n]["enqueue"]]
        engine_by = [n for n in games if per[n]["engine_emit"]]
        lua_by = [n for n in games if per[n]["lua_def"]]
        rows.append({
            "tag": tag,
            "engine": engine.get(tag, []),
            "in_upstream": None if upstream is None else (tag in upstream),
            "per_game": per,
            "strong_by": strong,
            "weak_by": weak,
            "enqueued_by": enqueued_by,
            "engine_emit_by": engine_by,
            "lua_def_by": lua_by,
        })

    rows.sort(key=lambda r: (r["in_upstream"] is not False, len(r["strong_by"]),
                             len(r["weak_by"]),
                             len(r["enqueued_by"]) + len(r["engine_emit_by"]), r["tag"]))
    shown = [r for r in rows if not args.unused or not r["strong_by"]]

    width = max(len(r["tag"]) for r in shown) if shown else 4
    header = "".join(f"{n[:7]:>9}" for n in games)
    print(f"engine tags: {len(engine)}   games: {', '.join(games)}"
          + (f"   upstream docs: {len(upstream)}" if upstream else ""))
    print(f"{'tag':<{width}}  {'up':>3}{header}   strong / weak / enqueue")
    for r in shown:
        marks = "".join(f"{MARK.get(r['per_game'][n]['verdict'], '?'):>9}" for n in games)
        who = ",".join(r["strong_by"]) or "-"
        weak = ",".join(r["weak_by"]) or "-"
        enq = ",".join(r["enqueued_by"] + r["engine_emit_by"]) or "-"
        up = "-" if r["in_upstream"] is None else ("yes" if r["in_upstream"] else "NO")
        print(f"{r['tag']:<{width}}  {up:>3}{marks}   {who} / {weak} / {enq}")

    print("\nlegend: S=脚本产出  L=Lua tags.X 消费(脚本路径)  l=Lua 定义但未见产出  "
          "E=引擎自己入队  .=无产出(ENGINE-ONLY)  -=引擎无原生面")
    print("        up=上游 tag 参考是否有该标签(NO = 引擎自造面)")
    if args.json:
        args.json.write_text(json.dumps({"games": games, "rows": rows}, indent=1, ensure_ascii=False),
                             encoding="utf-8")
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
