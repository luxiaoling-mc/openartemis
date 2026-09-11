// Smoke test: vendored Lua 5.1.5 links and runs (basic + our future engine
// table surface is added later in M4).
#include <cstdio>
#include <string>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
} // namespace

int main() {
    lua_State* L = luaL_newstate();
    if (!L) {
        std::fprintf(stderr, "luaL_newstate failed\n");
        return 1;
    }
    luaL_openlibs(L);
    
    // version string from globals
    lua_getglobal(L, "_VERSION");
    check(lua_isstring(L, -1) && std::string(lua_tostring(L, -1)) == "Lua 5.1",
          "_VERSION == Lua 5.1");
    lua_pop(L, 1);

    const int r = luaL_dostring(L, "res = (2 + 3 * 4) .. '|' .. ({1,2,3})[2]");
    check(r == 0, "dostring ok");
    if (r == 0) {
        lua_getglobal(L, "res");
        check(lua_isstring(L, -1) && std::string(lua_tostring(L, -1)) == "14|2", "lua math");
        lua_pop(L, 1);
    }
    // error path: status must be non-zero and the message on the stack
    const int bad = luaL_dostring(L, "error('boom')");
    check(bad != 0, "error returns nonzero");
    check(lua_isstring(L, -1) && std::string(lua_tostring(L, -1)).find("boom") != std::string::npos,
          "error message present");
    lua_pop(L, 1);
    // string.metatable coercion behavior (arithmetic
    // helpers; plain 5.1 does NOT have them)
    luaL_dostring(L, "s = '10' + 1");
    lua_getglobal(L, "s");
    check(lua_isnumber(L, -1) && lua_tonumber(L, -1) == 11.0, "5.1 numeric coercion");
    lua_pop(L, 1);

    lua_close(L);
    if (failures) {
        std::fprintf(stderr, "lua_smoke_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("lua_smoke_test: all ok\n");
    return 0;
}
