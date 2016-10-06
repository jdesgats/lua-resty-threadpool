/*
Copyright (c) 2016, Julien Desgats
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdint.h>
#include <string.h>
#include <lua.h>
#include <lauxlib.h>
#include <assert.h>

/* this library serializes all sorts of Lua variables on a byte stream. */

/*
 * TODO:
 * serialize more things
 * remove thread hack (rather inefficient)
 * benchmark, optimize
 * streaming serialization
 * be less platform dependant (endianness issues, remove Lua constants usage, ...)
 * test, test, and test
 */

#if (LUA_VERSION_NUM < 502)
static int lua_absindex (lua_State *L, int idx) {
  return (idx > 0 || idx <= LUA_REGISTRYINDEX) ?
      idx :
      lua_gettop(L) + 1 + idx;
}
#endif

#ifndef LUA_OK
# define LUA_OK 0
#endif

static void encodevalue(lua_State *L, int idx, luaL_Buffer *buf);

static int writer (lua_State *L, const void* b, size_t size, void* B) {
  (void)L;
  luaL_addlstring((luaL_Buffer*) B, (const char *)b, size);
  return 0;
}

/* serializes value on top into given buffer, the buffer must not be on the
** same state as L (because this function uses stack).
*/
static void encodevalue(lua_State *L, int idx, luaL_Buffer *buf) {
  idx = lua_absindex(L, idx);
  switch(lua_type(L, idx)) {
    case LUA_TNIL:
      luaL_addchar(buf, LUA_TNIL);
      break;
    case LUA_TBOOLEAN:
      luaL_addchar(buf, LUA_TBOOLEAN);
      luaL_addchar(buf, (char)lua_toboolean(L, idx));
      break;
    case LUA_TNUMBER: {
      lua_Number n = lua_tonumber(L, idx);
      luaL_addchar(buf, LUA_TNUMBER);
      luaL_addlstring(buf, (const char *)&n, sizeof(lua_Number));
      break;
    }
    case LUA_TSTRING: {
      size_t len;
      uint32_t serlen;
      const char *str = lua_tolstring(L, idx, &len);
      if (len > UINT32_MAX) {
          luaL_error(L, "string too long");
      }

      serlen = len; /* be sure of the size */
      luaL_addchar(buf, LUA_TSTRING);
      luaL_addlstring(buf, (const char *)&serlen, sizeof(uint32_t));
      luaL_addlstring(buf, str, len);
      break;
    }
    case LUA_TTABLE:
      if (lua_getmetatable(L, idx)) {
          /* TODO: why not ? */
          luaL_error(L, "cannot serialize table with metatable");
      }
      luaL_addchar(buf, LUA_TTABLE);
      lua_pushnil(L);
      while(lua_next(L, idx) != 0) {
        encodevalue(L, -2, buf);
        encodevalue(L, -1, buf);
        lua_pop(L, 1);
      }
      /* signal end of table (key cannot be nil) */
      luaL_addchar(buf, LUA_TNIL);
      break;
    case LUA_TFUNCTION: {
      luaL_Buffer dumpbuf;
      lua_Debug ar;
      uint32_t dumplen;

      lua_pushvalue(L, idx);
      if (lua_iscfunction(L, idx)) {
        luaL_error(L, "cannot serialize C function");
      }
      if (lua_getinfo(L, ">u", &ar) == 0 || ar.nups > 0) {
        luaL_error(L, "cannot serialize function with upvalues");
      }
      
      /* we need to get the size of the dump, before dump itself:
         use a separate buffer for dumping */
      /* TODO: save function name */
      lua_pushvalue(L, idx);
      luaL_buffinit(L, &dumpbuf);
      if (lua_dump(L, writer, &dumpbuf) != 0) {
        luaL_error(L, "unable to dump function");
      }
      luaL_pushresult(&dumpbuf);

      dumplen = lua_objlen(L, -1);
      luaL_addchar(buf, LUA_TFUNCTION);
      luaL_addlstring(buf, (const char *)&dumplen, sizeof(uint32_t));
      lua_xmove(L, buf->L, 1);
      luaL_addvalue(buf);
      lua_pop(L, 1); /* pops function */
      break;
    }
    default:
      luaL_error(L, "cannot serialize %s", luaL_typename(L, idx));
  }
}

#define checkbuffer(cur, end, n) do { \
  if (cur + n > end) luaL_error(L, "wrong code"); \
} while(0)

static const char* decodevalue(lua_State *L, const char *buf, const char *end) {
  checkbuffer(buf, end, 1);
  switch (*buf++) {
    case LUA_TNIL:
      lua_pushnil(L);
      break;
    case LUA_TBOOLEAN:
      checkbuffer(buf, end, 1);
      lua_pushboolean(L, *buf != 0);
      buf++;
      break;
    case LUA_TNUMBER:
      checkbuffer(buf, end, sizeof(lua_Number));
      lua_pushnumber(L, *(const lua_Number *)buf);
      buf += sizeof(lua_Number);
      break;
    case LUA_TSTRING: {
      uint32_t serlen;
      checkbuffer(buf, end, sizeof(uint32_t));
      serlen = *(const uint32_t*)(buf);
      buf += sizeof(uint32_t);
      checkbuffer(buf, end, serlen);
      lua_pushlstring(L, buf, serlen);
      buf += serlen;
      break;
    }
    case LUA_TTABLE:
      lua_newtable(L);
      checkbuffer(buf, end, 1);
      while (*buf != LUA_TNIL) {
        buf = decodevalue(L, buf, end);
        checkbuffer(buf, end, 1);
        buf = decodevalue(L, buf, end);
        checkbuffer(buf, end, 1);
        lua_settable(L, -3);
      }
      buf++; /* skip the final TNIL token */
      break;
    case LUA_TFUNCTION: {
      uint32_t dumplen;
      checkbuffer(buf, end, sizeof(uint32_t));
      dumplen = *(const uint32_t*)(buf);
      buf += sizeof(uint32_t);
      checkbuffer(buf, end, dumplen);
      if (luaL_loadbuffer(L, buf, dumplen, "unserialized") != LUA_OK) {
        luaL_error(L, "failed to load function");
      }
      buf += dumplen;
      break;
    }
    default:
      luaL_error(L, "wrong type identifier");
  }
  assert(buf <= end);
  return buf;
}

/* public API */
/* serialize value at index idx, pushes resulting string into the stack */
void luaser_encode(lua_State *L, int idx)
{
  luaL_Buffer buf;
  lua_State  *bufL;

  idx = lua_absindex(L, idx);
  bufL = lua_newthread(L);
  luaL_buffinit(bufL, &buf);
  encodevalue(L, idx, &buf);

  luaL_pushresult(&buf);
  lua_xmove(bufL, L, 1);
  lua_remove(L, -2); /* remove the thread */
}

/* deserializes the given value and pushes it ot the stack */
void luaser_decode(lua_State *L, const char *buf, size_t len)
{
  const char *end = buf + len;  
  decodevalue(L, buf, end);
}
