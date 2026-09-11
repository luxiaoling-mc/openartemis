#!/usr/bin/env python3
"""Census the tag names used by an Artemis project's .ast story scripts.

An .ast file is one plain-text table. The head of the table holds one *block*
per script step; each block is a list of tag invocations, and a block may carry
``delay={ vl1={ ... } }`` timing lanes whose rows are tag invocations too.
``text={}`` and ``label={}`` are sibling keys of the blocks and hold dialogue
strings and label targets, not tags::

    ast={
      {
        {"savetitle",text="..."},
        {"bg",file="catch04",id=9,lv=9,path=":cg/",time=500},
        {"text"},
        crc="ed25933c",
        lang="lang_00000",
        delay={
          vl1={ {"fg",ch="...",face="a0041"} },
        },
      },
      ...
      text={ [1]={ja={ {"..."} }}, ... },
      label={ label_skip={block=1,label=8} },
    }

This tool walks that tree, records the name of every tag row, and prints a
frequency table plus the parameter names each tag is called with, so the
engine's tag surface can be diffed against what a real game actually uses.

Usage::

    python tools/tagcensus.py <project-root>
    python tools/tagcensus.py <project-root> --all-params
    python tools/tagcensus.py <project-root> --samples uitrans,quake
    python tools/tagcensus.py <project-root> --keys --json build/tags.json
"""

from __future__ import annotations

import argparse
import collections
import json
import re
import sys
from pathlib import Path

# A tag row's name is an ASCII identifier (or a `/close` tag); dialogue rows are
# bare strings, and prose that happens to be ASCII ("However...", "Yes.") is
# rejected by the no-punctuation rule instead of by skipping whole language
# tables — .ast v2 mixes prose and tag rows inside one language table (e.g.
# `{"rt2"}` after a line).
TAG_NAME_RE = re.compile(r"^(?:[A-Za-z_@][A-Za-z0-9_@]*|/[A-Za-z_][A-Za-z0-9_]*)$")

IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
INDEX_RE = re.compile(r"\[[^\]]*\]")
NAME_RE = re.compile(r'\{\s*(?:"([^"]+)"|([A-Za-z_][A-Za-z0-9_]*))')
PARAM_RE = re.compile(r"[,{]\s*([A-Za-z_][A-Za-z0-9_]*)\s*=")


def skip_string(text: str, i: int) -> int:
    """Index just past the quoted string starting at text[i]."""
    quote = text[i]
    i += 1
    while i < len(text):
        if text[i] == "\\":
            i += 2
            continue
        if text[i] == quote:
            return i + 1
        i += 1
    return i


def skip_group(text: str, i: int) -> int:
    """Index just past the {...} group starting at text[i]."""
    depth = 0
    while i < len(text):
        ch = text[i]
        if ch in "\"'":
            i = skip_string(text, i)
            continue
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return i


def skip_value(text: str, i: int) -> int:
    """Index just past the value starting at text[i] (group, string, or bare token)."""
    while i < len(text) and text[i] in " \t\r\n":
        i += 1
    if i >= len(text):
        return i
    if text[i] == "{":
        return skip_group(text, i)
    if text[i] in "\"'":
        return skip_string(text, i)
    start = i
    while i < len(text) and text[i] not in ",}\r\n":
        i += 1
    return max(i, start + 1)


def root_span(text: str) -> tuple[int, int] | None:
    """[start, end) offsets of the value of the file's ``ast = {...}`` table.

    Two dialects exist in the wild: files that *are* the table (FPM) and files
    with a version header before it (``astver = 2.0`` / ``astname = "ast"``).
    Both spell the table key ``ast``, so key off that and fall back to the first
    table in the file.
    """
    m = re.search(r"\bast\s*=\s*\{", text) or re.search(r"=\s*\{", text)
    if not m:
        return None
    i = text.index("{", m.start())
    end = skip_group(text, i)
    return (i + 1, end - 1)


def entries(text: str, lo: int, hi: int):
    """Yield ('row', None, start, group) and ('key', name, start, group) in [lo, hi).

    ``group`` is the raw source of the ``{...}``, or None for a scalar value.
    """
    i = lo
    while i < hi:
        ch = text[i]
        if ch in " \t\r\n,":
            i += 1
            continue
        if ch == "{":
            end = skip_group(text, i)
            yield ("row", None, i, text[i:end])
            i = end
            continue
        m = IDENT_RE.match(text, i) or INDEX_RE.match(text, i)
        if not m:
            i = skip_value(text, i)
            continue
        name = m.group(0)
        j = m.end()
        while j < hi and text[j] in " \t\r\n":
            j += 1
        if j >= hi or text[j] != "=":
            i = skip_value(text, j)
            continue
        j += 1
        while j < hi and text[j] in " \t\r\n":
            j += 1
        if j < hi and text[j] == "{":
            end = skip_group(text, j)
            yield ("key", name, j, text[j:end])
            i = end
        else:
            yield ("key", name, j, None)
            i = skip_value(text, j)


def tag_name(group: str) -> str | None:
    """Name of a tag row, or None when the group is prose/a container.

    A tag row is ``{"name", ...}`` (or ``{name, ...}``). Rejected:
    * a bare ``key=value`` list (delay lane table, ``{crc="..", lang=".."}``),
    * a quoted first element that is not an ASCII tag name — i.e. a dialogue
      string, which is how prose rows look in ``text``/``ja``/``tw`` tables,
    * a quoted name followed by another string (``{"月姫","月姬"}`` name pairs).
    """
    m = NAME_RE.match(group)
    if not m:
        return None
    if m.group(1):
        if not TAG_NAME_RE.match(m.group(1)):
            return None
        after = group[m.end() :].lstrip()
        if after.startswith(","):
            rest = after[1:].lstrip()
            if rest.startswith('"') or rest.startswith("'"):
                return None  # a name/translation pair, not a tag row
        return m.group(1)
    after = group[m.end() :].lstrip()
    if after.startswith("="):
        return None
    return m.group(2)


def lineno(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def walk(text: str, lo: int, hi: int, visit_tag, visit_key):
    """Walk the table in [lo, hi): report tag rows, descend into containers."""
    for kind, name, start, group in entries(text, lo, hi):
        if group is None:
            continue
        if kind == "key":
            visit_key(name)
            walk(text, start + 1, start + len(group) - 1, visit_tag, visit_key)
            continue
        tag = tag_name(group)
        if tag is None:
            walk(text, start + 1, start + len(group) - 1, visit_tag, visit_key)
        else:
            visit_tag(tag, start, group)


class Census:
    def __init__(self) -> None:
        self.total: collections.Counter = collections.Counter()
        self.params: dict[str, collections.Counter] = collections.defaultdict(collections.Counter)
        self.keys: collections.Counter = collections.Counter()
        self.per_file: dict[str, collections.Counter] = {}
        self.samples: dict[str, list[tuple[str, int, str]]] = collections.defaultdict(list)
        self.rows = 0


def census(root: Path) -> tuple[Census, list[str]]:
    result = Census()
    skipped: list[str] = []
    for path in sorted(root.rglob("*.ast")):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:  # pragma: no cover - filesystem dependent
            skipped.append(f"{path}: {exc}")
            continue
        span = root_span(text)
        if span is None:
            skipped.append(f"{path}: no top-level table")
            continue
        rel = str(path.relative_to(root))
        local: collections.Counter = collections.Counter()

        def visit_tag(tag: str, start: int, group: str) -> None:
            local[tag] += 1
            result.rows += 1
            result.params[tag].update(PARAM_RE.findall(group))
            if len(result.samples[tag]) < 6:
                result.samples[tag].append((rel, lineno(text, start), group.replace("\n", " ")))

        def visit_key(name: str) -> None:
            result.keys[name] += 1

        walk(text, span[0], span[1], visit_tag, visit_key)
        result.per_file[rel] = local
        result.total.update(local)
    return result, skipped


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("root", type=Path, help="project root (the directory holding system.ini)")
    ap.add_argument("--json", type=Path, help="also write the full census as JSON")
    ap.add_argument("--files", help="also list per-file counts for these tags (comma separated)")
    ap.add_argument("--params", metavar="TAG", help="list parameter names used by these tags")
    ap.add_argument("--all-params", action="store_true", help="list parameters for every tag")
    ap.add_argument("--samples", metavar="TAG", help="show example rows for these tags")
    ap.add_argument("--all-samples", action="store_true", help="show example rows for every tag")
    ap.add_argument("--keys", action="store_true", help="list keyed table names seen in the tree")
    ap.add_argument("--min", type=int, default=1, help="minimum occurrences to list")
    args = ap.parse_args()

    result, skipped = census(args.root)
    print(f"{len(result.per_file)} .ast files, {result.rows} tag rows, {len(result.total)} distinct tags\n")
    for name, count in result.total.most_common():
        if count >= args.min:
            print(f"{count:8d}  {name}")

    for line in skipped:
        print(f"!! {line}", file=sys.stderr)

    def show_params(name: str) -> None:
        print(f"\n== params: {name} ({result.total.get(name, 0)} uses) ==")
        for p, c in result.params.get(name, {}).most_common():
            print(f"{c:8d}  {p}")

    def show_samples(name: str) -> None:
        print(f"\n== samples: {name} ({result.total.get(name, 0)} uses) ==")
        for f, line, row in result.samples.get(name, []):
            print(f"  {f}:{line}: {row}")

    if args.keys:
        print("\n== keyed tables ==")
        for k, c in result.keys.most_common():
            print(f"{c:8d}  {k}")

    for name in [w.strip() for w in (args.params or "").split(",") if w.strip()]:
        show_params(name)
    for name in [w.strip() for w in (args.samples or "").split(",") if w.strip()]:
        show_samples(name)
    if args.all_params:
        for name, _ in result.total.most_common():
            show_params(name)
    if args.all_samples:
        for name, _ in result.total.most_common():
            show_samples(name)

    if args.files:
        for name in [w.strip() for w in args.files.split(",") if w.strip()]:
            print(f"\n== files: {name} ==")
            hits = [(f, c[name]) for f, c in result.per_file.items() if c[name]]
            for f, n in sorted(hits, key=lambda kv: (-kv[1], kv[0]))[:40]:
                print(f"{n:8d}  {f}")
            print(f"  ({len(hits)} files)")

    if args.json:
        payload = {
            "root": str(args.root),
            "totals": dict(result.total.most_common()),
            "params": {t: dict(c) for t, c in result.params.items()},
            "keys": dict(result.keys.most_common()),
            "per_file": {f: dict(c) for f, c in result.per_file.items()},
        }
        args.json.write_text(json.dumps(payload, indent=1, ensure_ascii=False), encoding="utf-8")
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
