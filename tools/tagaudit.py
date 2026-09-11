#!/usr/bin/env python3
"""Audit the engine's tag surface against what a real game actually uses.

The engine handles a tag either *natively* (a branch in the interpreter, a
runtime event handler, an array of marker tags, or `register_engine_tag`) or by
*delegation* to the game's Lua `tags` table through `e:setTagFilter`. This tool
builds both sides and prints a diff, so the deletable half of the native surface
is evidence-backed instead of guessed:

* **engine side** — every tag name compared or listed in `src/core/**`:
  `tag == "x"`, `e.tag == "x"`, `ev->tag == "x"`, `register_engine_tag("x")`,
  and string arrays whose name mentions tag/builtin/config.
* **game side** — three independent producers:
  1. `.ast` script tables (via `tagcensus`),
  2. `.iet` / decoded ASB scripts, `[tag ...]` bracket lines,
  3. Lua: `function tags.x` definitions plus every quoted occurrence of the name.

Verdicts:

* `used`      — the game defines or emits the tag, keep the engine path.
* `shadowed`  — the game defines `tags.x` *and* the engine has a native branch.
  The Lua filter runs first; a handler returning a truthy value consumes the tag
  and the native branch never runs. Worth re-reading before deleting: a handler
  that returns nil/false falls through to the engine (see interpreter.cpp
  apply-tag filter rule).
* `ENGINE-ONLY` — nothing in the game produces it. Prime deletion candidate.

Usage::

    python tools/tagaudit.py --src src/core --game /path/to/project
    python tools/tagaudit.py --src src/core --game P --verdict ENGINE-ONLY -v
    python tools/tagaudit.py --src src/core --game P --json build/tagaudit.json
"""

from __future__ import annotations

import argparse
import collections
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tagcensus  # noqa: E402

TAG_COMPARE_RE = re.compile(
    r'(?:[A-Za-z_][A-Za-z0-9_]*(?:->|\.))*tag\s*==\s*"([^"\\]+)"'
)
REGISTER_RE = re.compile(r'register_engine_tag\(\s*\n?\s*"([^"\\]+)"')
EMIT_RE = re.compile(
    r'(?:enqueue_tag|push_tag|Event::custom|custom_event)\(\s*\n?\s*"([^"\\]+)"'
)
ARRAY_RE = re.compile(r"static\s+const\s+char\s*\*\s*(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\};", re.S)
STRING_RE = re.compile(r'"([^"\\]*)"')
LUA_TAG_DEF_RE = re.compile(r"function\s+tags\.([A-Za-z_][A-Za-z0-9_]*)")
# Producers: the game's Lua emits tags through its `tag{}`/`e:tag{}` helpers, and
# compiled/plain scripts write `[name ...]` at the start of a line. The lookbehind
# keeps `estag{}` (the game's deferred tag *queue*, which dispatches to Lua
# functions first) and `eqtag{}` out of the plain-tag producer set.
LUA_TAG_CALL_RE = re.compile(
    r'(?<![A-Za-z0-9_])(?:e:)?(?:enqueue)?[Tt]ag\s*\{\s*"([A-Za-z_@/][A-Za-z0-9_./@]*)"'
)
LUA_EQTAG_RE = re.compile(
    r'(?<![A-Za-z0-9_])(?:e:)?(?:en)?[Ee][Qq][Tt]ag\s*\{\s*"([A-Za-z_@/][A-Za-z0-9_./@]*)"'
)
IET_LINE_TAG_RE = re.compile(r"^\s*\[([A-Za-z_@/][A-Za-z0-9_./@]*)\b")
LUA_STRING_BRACKET_RE = re.compile(r'["\']\s*\[([A-Za-z_@/][A-Za-z0-9_./@]*)\b')


def engine_surface(src: Path):
    """tag -> (native handler sites, engine-emitted sites)."""
    sites: dict[str, list[str]] = collections.defaultdict(list)
    emitted: dict[str, list[str]] = collections.defaultdict(list)
    for path in sorted(src.rglob("*")):
        if path.suffix not in (".cpp", ".h"):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        rel = str(path)
        for lineno, line in enumerate(text.splitlines(), 1):
            for m in TAG_COMPARE_RE.finditer(line):
                sites[m.group(1)].append(f"{rel}:{lineno}")
            for m in REGISTER_RE.finditer(line):
                sites[m.group(1)].append(f"{rel}:{lineno}")
            for m in EMIT_RE.finditer(line):
                emitted[m.group(1)].append(f"{rel}:{lineno}")
        for m in ARRAY_RE.finditer(text):
            name, body = m.group(1), m.group(2)
            if not re.search(r"(?i)tag|builtin|config", name):
                continue
            lineno = text.count("\n", 0, m.start()) + 1
            for sm in STRING_RE.finditer(body):
                if sm.group(1):
                    sites[sm.group(1)].append(f"{rel}:{lineno} ({name})")
    return sites, emitted


def game_surface(game: Path, asb_dir: Path | None):
    """Lua definitions, Lua tag-call producers, script bracket tags, .ast rows.

    Returns ``(lua_defs, lua_emit, lua_mentions, bracket, bracket_sites, ast,
    detail)`` where ``bracket`` is the combined "produced" counter and ``detail``
    splits it into ``lua_tag`` (``tag{"x"}``), ``lua_str`` (``"[x]"`` inside a Lua
    string) and ``iet`` (a ``[x ...]`` script line) so callers can weigh strong
    producers against weak mentions.
    """
    lua_defs: dict[str, list[str]] = collections.defaultdict(list)
    lua_emit: dict[str, list[str]] = collections.defaultdict(list)
    lua_mentions: dict[str, list[str]] = collections.defaultdict(list)
    detail: dict[str, collections.Counter] = {
        "lua_tag": collections.Counter(),
        "lua_str": collections.Counter(),
        "iet": collections.Counter(),
    }
    bracket: collections.Counter = collections.Counter()
    bracket_sites: dict[str, list[str]] = collections.defaultdict(list)

    lua_files = sorted(game.rglob("*.lua"))
    for path in lua_files:
        text = path.read_text(encoding="utf-8", errors="replace")
        rel = str(path.relative_to(game))
        for lineno, line in enumerate(text.splitlines(), 1):
            for m in LUA_TAG_DEF_RE.finditer(line):
                lua_defs[m.group(1)].append(f"{rel}:{lineno}")
            for m in LUA_TAG_CALL_RE.finditer(line):
                lua_emit[m.group(1)].append(f"{rel}:{lineno}")
                detail["lua_tag"][m.group(1)] += 1
                bracket[m.group(1)] += 1
                if len(bracket_sites[m.group(1)]) < 6:
                    bracket_sites[m.group(1)].append(f"{rel}:{lineno}")
            for m in LUA_STRING_BRACKET_RE.finditer(line):
                # A bracketed tag embedded in a Lua string.
                detail["lua_str"][m.group(1)] += 1
                bracket[m.group(1)] += 1
        quoted = set(re.findall(r'["\']([A-Za-z_@/][A-Za-z0-9_@/]*)["\']', text))
        for name in quoted:
            lua_mentions[name].append(rel)

    iet_files = sorted(game.rglob("*.iet"))
    if asb_dir:
        iet_files += sorted(asb_dir.rglob("*.iet"))
    for path in iet_files:
        text = path.read_text(encoding="utf-8", errors="replace")
        rel = str(path)
        for lineno, line in enumerate(text.splitlines(), 1):
            if not line.lstrip().startswith("["):
                continue
            # Several tags may share one line (`[if ..][/if]`).
            for m in re.finditer(r"\[([A-Za-z_@/][A-Za-z0-9_./@]*)\b", line):
                detail["iet"][m.group(1)] += 1
                bracket[m.group(1)] += 1
                if len(bracket_sites[m.group(1)]) < 6:
                    bracket_sites[m.group(1)].append(f"{rel}:{lineno}")

    ast_census, _ = tagcensus.census(game)
    return lua_defs, lua_emit, lua_mentions, bracket, bracket_sites, ast_census, detail


def upstream_tags(docs: Path) -> set[str]:
    """Tag names documented by the upstream tag reference (`tag/*.md`).

    One file per tag, possibly nested (`tag/graphics/trans.md`). `readme` is the
    index, not a tag.
    """
    names = set()
    for path in docs.rglob("*"):
        if path.is_file() and path.suffix.lower() in (".md", ".txt"):
            stem = path.stem
            if stem.lower() not in ("readme", "index"):
                names.add(stem)
    return names


def verdict_for(tag: str, engine: bool, engine_emit: bool, lua_def: bool, emitted: bool) -> str:
    if engine_emit and not emitted:
        return "engine-produced"
    if engine and lua_def:
        return "shadowed" if emitted else "shadowed?"
    if engine and emitted:
        return "used"
    if engine:
        return "ENGINE-ONLY"
    return "game-only" if (lua_def or emitted) else "-"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--src", type=Path, default=Path("src/core"), help="engine source root")
    ap.add_argument("--game", type=Path, required=True, help="unpacked game project root")
    ap.add_argument("--asb-iet", type=Path, help="directory of `asb extract` output to scan")
    ap.add_argument("--verdict", help="only show these verdicts (comma separated)")
    ap.add_argument("-v", "--verbose", action="store_true", help="show evidence sites")
    ap.add_argument("--json", type=Path, help="also write the audit as JSON")
    args = ap.parse_args()

    engine, engine_emits = engine_surface(args.src)
    lua_defs, lua_emit, lua_mentions, bracket, bracket_sites, ast_census, detail = game_surface(
        args.game, args.asb_iet
    )

    rows = []
    names = sorted(set(engine) | set(engine_emits) | set(lua_defs) | set(bracket) | set(ast_census.total))
    for tag in names:
        engine_sites = engine.get(tag, [])
        emits = engine_emits.get(tag, [])
        defs = lua_defs.get(tag, [])
        ast_rows = ast_census.total.get(tag, 0)
        iet_rows = bracket.get(tag, 0)
        mentions = lua_mentions.get(tag, [])
        emitted = bool(ast_rows or iet_rows or mentions)
        rows.append(
            {
                "tag": tag,
                "verdict": verdict_for(tag, bool(engine_sites), bool(emits), bool(defs), emitted),
                "engine": engine_sites,
                "engine_emit": emits,
                "lua_def": defs,
                "lua_emit": lua_emit.get(tag, [])[:6],
                "ast": ast_rows,
                "iet": iet_rows,
                "lua_mention": len(mentions),
                "lua_files": mentions[:4],
                "iet_sites": bracket_sites.get(tag, []),
            }
        )

    order = {
        "ENGINE-ONLY": 0,
        "shadowed": 1,
        "shadowed?": 2,
        "engine-produced": 3,
        "used": 4,
        "game-only": 5,
        "-": 6,
    }
    rows.sort(key=lambda r: (order.get(r["verdict"], 9), r["tag"]))

    wanted = {v.strip() for v in (args.verdict or "").split(",") if v.strip()}
    shown = [r for r in rows if not wanted or r["verdict"] in wanted]

    print(f"engine tags: {len(engine)}   lua tags: {len(lua_defs)}   "
          f"script tags: {len(ast_census.total)}   produced tags: {len(bracket)}\n")
    print(f"{'verdict':<16}{'tag':<22}{'engine':>7}{'lua':>5}{'ast':>7}{'emit':>6}{'lit':>5}")
    for r in shown:
        print(f"{r['verdict']:<16}{r['tag']:<22}{len(r['engine']):>7}{len(r['lua_def']):>5}"
              f"{r['ast']:>7}{r['iet']:>6}{r['lua_mention']:>5}")
        if args.verbose:
            if r["engine"]:
                print(f"    engine : {', '.join(r['engine'][:6])}")
            if r["engine_emit"]:
                print(f"    emits  : {', '.join(r['engine_emit'][:4])}")
            if r["lua_def"]:
                print(f"    lua def: {', '.join(r['lua_def'][:4])}")
            if r["lua_emit"]:
                print(f"    lua tag: {', '.join(r['lua_emit'][:4])}")
            if r["iet_sites"]:
                print(f"    script : {', '.join(r['iet_sites'][:4])}")
            if r["lua_files"]:
                print(f"    lua lit: {', '.join(r['lua_files'])}")

    if args.json:
        args.json.write_text(json.dumps({"rows": rows}, indent=1, ensure_ascii=False), encoding="utf-8")
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
