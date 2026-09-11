# Lua 5.1.5 (vendored)

Source: <https://www.lua.org/ftp/lua-5.1.5.tar.gz> (Lua License, MIT-style:
`etc/luac.c` and `lua.c` terms apply; see `doc/readme.html` inside).
Vendored because the engine must match Lua 5.1 semantics exactly
(the game frameworks in the target ecosystem are written for Lua 5.1).

Files are unmodified upstream Lua **except one local patch**, which the build
depends on:

- `src/lgc.c` — `traversetable()`'s `__mode` scan spells the `strchr` calls out
  as a loop. On Android arm64 with `_FORTIFY_SOURCE` (the NDK default) bionic's
  `__strchr_chk` aborts that call — "FORTIFY: strchr: prevented read past end of
  buffer" — killing the process in the middle of a GC step reached from
  `luaL_openlibs`, before any script runs. The loop is the same scan with the
  same result; a Lua `TString` always carries its NUL terminator.

Build integration lives in `src/core/CMakeLists.txt` via the `lua51` interface
target; the engine layer decides which standard libraries to open at runtime
(the engine restricts `os`/`io` — see the `OA_LUA_STDLIB` row in
`docs/TESTING.md` and `src/core/runtime/runtime_lua.cpp` for the rationale).
