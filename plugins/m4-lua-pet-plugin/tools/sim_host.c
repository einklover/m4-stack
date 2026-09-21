/* Firmware-like Lua host: 2e6 instruction budget, unix sys.time, stub gui/fs.
 * Not shipped in the .m4x. */
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { kHookStep = 10000, kInstrBudget = 2000000 };

static unsigned instr_spent;
static int violated;
static const char *reason;
static char persist_buf[4096];
static int persist_len;
static lua_Number fake_time = 1700000000.0;

static void count_hook(lua_State *L, lua_Debug *ar) {
  (void)ar;
  instr_spent += (unsigned)kHookStep;
  if (instr_spent >= (unsigned)kInstrBudget) {
    violated = 1;
    reason = "lua_instr_limit";
    luaL_error(L, "instruction budget exceeded");
  }
}

static int l_nop(lua_State *L) {
  (void)L;
  return 0;
}
static int l_zero(lua_State *L) {
  lua_pushinteger(L, 0);
  return 1;
}
static int l_width(lua_State *L) {
  lua_pushinteger(L, 480);
  return 1;
}
static int l_height(lua_State *L) {
  lua_pushinteger(L, 800);
  return 1;
}
static int l_textWidth(lua_State *L) {
  size_t n = 0;
  luaL_checklstring(L, 2, &n);
  lua_pushinteger(L, (lua_Integer)(n * 8));
  return 1;
}
static int l_lineHeight(lua_State *L) {
  lua_pushinteger(L, 16);
  return 1;
}
static int l_sys_time(lua_State *L) {
  lua_pushnumber(L, fake_time);
  return 1;
}
static int l_sys_millis(lua_State *L) {
  lua_pushnumber(L, fake_time * 1000.0);
  return 1;
}
static int l_fs_write(lua_State *L) {
  size_t n = 0;
  const char *s = luaL_checklstring(L, 2, &n);
  if (n >= sizeof(persist_buf)) n = sizeof(persist_buf) - 1;
  memcpy(persist_buf, s, n);
  persist_buf[n] = 0;
  persist_len = (int)n;
  lua_pushboolean(L, 1);
  return 1;
}
static int l_fs_read(lua_State *L) {
  if (persist_len <= 0) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushlstring(L, persist_buf, (size_t)persist_len);
  return 1;
}

static void open_stubs(lua_State *L) {
  lua_newtable(L);
  lua_pushcfunction(L, l_width);
  lua_setfield(L, -2, "width");
  lua_pushcfunction(L, l_height);
  lua_setfield(L, -2, "height");
  lua_pushcfunction(L, l_nop);
  lua_setfield(L, -2, "clear");
  lua_pushcfunction(L, l_nop);
  lua_setfield(L, -2, "drawText");
  lua_pushcfunction(L, l_textWidth);
  lua_setfield(L, -2, "textWidth");
  lua_pushcfunction(L, l_lineHeight);
  lua_setfield(L, -2, "lineHeight");
  lua_pushcfunction(L, l_nop);
  lua_setfield(L, -2, "drawRect");
  lua_pushcfunction(L, l_nop);
  lua_setfield(L, -2, "fillRect");
  lua_pushcfunction(L, l_nop);
  lua_setfield(L, -2, "drawLine");
  lua_pushcfunction(L, l_nop);
  lua_setfield(L, -2, "refresh");
  lua_setglobal(L, "gui");

  lua_newtable(L);
  lua_pushcfunction(L, l_sys_time);
  lua_setfield(L, -2, "time");
  lua_pushcfunction(L, l_sys_millis);
  lua_setfield(L, -2, "millis");
  lua_setglobal(L, "sys");

  lua_newtable(L);
  lua_pushcfunction(L, l_fs_write);
  lua_setfield(L, -2, "writeFile");
  lua_pushcfunction(L, l_fs_read);
  lua_setfield(L, -2, "readFile");
  lua_setglobal(L, "fs");
}

static int call_named(lua_State *L, const char *name) {
  instr_spent = 0;
  violated = 0;
  reason = NULL;
  lua_sethook(L, count_hook, LUA_MASKCOUNT, kHookStep);
  lua_getglobal(L, name);
  if (!lua_isfunction(L, -1)) {
    fprintf(stderr, "missing %s\n", name);
    lua_pop(L, 1);
    return 1;
  }
  int rc = lua_pcall(L, 0, 0, 0);
  printf("%s rc=%d instr=%u violated=%d reason=%s\n", name, rc, instr_spent,
         violated, reason ? reason : "-");
  if (rc != LUA_OK) {
    fprintf(stderr, "  err: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    return 1;
  }
  return 0;
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "main.lua";
  if (argc > 2) fake_time = atof(argv[2]);
  if (argc > 3) {
    strncpy(persist_buf, argv[3], sizeof(persist_buf) - 1);
    persist_len = (int)strlen(persist_buf);
  }

  lua_State *L = luaL_newstate();
  if (!L) return 3;
  luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
  lua_pop(L, 1);
  luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
  lua_pop(L, 1);
  luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
  lua_pop(L, 1);
  luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
  lua_pop(L, 1);
  luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
  lua_pop(L, 1);
  open_stubs(L);

  instr_spent = 0;
  lua_sethook(L, count_hook, LUA_MASKCOUNT, kHookStep);
  int rc = luaL_loadfile(L, path);
  if (rc != LUA_OK) {
    fprintf(stderr, "load: %s\n", lua_tostring(L, -1));
    return 1;
  }
  rc = lua_pcall(L, 0, 0, 0);
  printf("chunk rc=%d instr=%u violated=%d reason=%s\n", rc, instr_spent,
         violated, reason ? reason : "-");
  if (rc != LUA_OK) {
    fprintf(stderr, "  err: %s\n", lua_tostring(L, -1));
    return 1;
  }
  if (call_named(L, "init")) return 1;
  if (call_named(L, "draw")) return 1;
  lua_close(L);
  printf("OK persist_len=%d\n", persist_len);
  return 0;
}
