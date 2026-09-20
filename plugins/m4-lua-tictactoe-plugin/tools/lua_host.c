/* Tiny Lua 5.4 host for plugin unit tests. Not shipped in the .m4x. */
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include <stdio.h>

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: lua_host script.lua [args]\n");
    return 2;
  }
  lua_State* L = luaL_newstate();
  if (!L) return 3;
  /* Match M4xLuaHost openSafeLibs (no io/os/package/debug — those .c files
     are stripped from firmware/lib/Lua). */
  luaL_requiref(L, LUA_GNAME, luaopen_base, 1); lua_pop(L, 1);
  luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(L, 1);
  luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
  luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(L, 1);
  luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1); lua_pop(L, 1);
  luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1); lua_pop(L, 1);
  lua_createtable(L, argc - 2, 0);
  for (int i = 2; i < argc; i++) {
    lua_pushstring(L, argv[i]);
    lua_rawseti(L, -2, i - 1);
  }
  lua_setglobal(L, "arg");
  lua_pushstring(L, argv[1]);
  lua_setglobal(L, "arg0");
  /* test_game.lua uses arg[0] for directory */
  lua_getglobal(L, "arg");
  lua_pushstring(L, argv[1]);
  lua_rawseti(L, -2, 0);
  lua_pop(L, 1);
  if (luaL_dofile(L, argv[1]) != LUA_OK) {
    fprintf(stderr, "%s\n", lua_tostring(L, -1));
    lua_close(L);
    return 1;
  }
  lua_close(L);
  return 0;
}
