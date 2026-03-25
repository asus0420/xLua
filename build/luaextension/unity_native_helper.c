// Assets/Plugins/iOS/unity_native_helper.c
#include "lua.h"
#include "lauxlib.h"
#include <stdint.h>
#include <limits.h>

/* =========================================================
 * helpers
 * ========================================================= */

static inline uint8_t* check_ptr(lua_State* L, int idx) {
    void* p = lua_touserdata(L, idx);
    if (!p) {
        luaL_error(L, "invalid or null pointer");
    }
    return (uint8_t*)p;
}

/* =========================================================
 * read / write primitives (NO memcpy)
 * ========================================================= */

// read_byte(ptr) -> int
static int l_read_byte(lua_State* L) {
    uint8_t* p = check_ptr(L, 1);
    lua_pushinteger(L, *p);
    return 1;
}

// write_byte(ptr, val)
static int l_write_byte(lua_State* L) {
    uint8_t* p = check_ptr(L, 1);
    *p = (uint8_t)luaL_checkinteger(L, 2);
    return 0;
}

// read_int32(ptr) -> int
static int l_read_int32(lua_State* L) {
    uint8_t* p = check_ptr(L, 1);
    int32_t v = *(int32_t*)p;   // 直接解引用
    lua_pushinteger(L, v);
    return 1;
}

// write_int32(ptr, val)
static int l_write_int32(lua_State* L) {
    uint8_t* p = check_ptr(L, 1);
    *(int32_t*)p = (int32_t)luaL_checkinteger(L, 2);
    return 0;
}

/* =========================================================
 * pointer math
 * ========================================================= */

// offset(ptr, byte_offset) -> new_ptr
static int l_ptr_offset(lua_State* L) {
    uint8_t* base = check_ptr(L, 1);
    lua_Integer off = luaL_checkinteger(L, 2);

    // 不做 off < 0 的保护：语义交给调用方
    lua_pushlightuserdata(L, base + off);
    return 1;
}

/* =========================================================
 * bulk read
 * ========================================================= */

// read_bytes(ptr, count) -> b1, b2, ... (multi return)
// 仅用于小 count（Lua string.byte 风格）
static int l_read_bytes(lua_State* L) {
    uint8_t* p = check_ptr(L, 1);
    lua_Integer count = luaL_checkinteger(L, 2);

    if (count <= 0)
        return 0;

    // ---- 返回值上限（防 Lua 栈爆）----
    const lua_Integer MAX_RET = 64;  // 可按项目调整
    if (count > MAX_RET) {
        return luaL_error(
            L,
            "read_bytes count(%d) exceeds limit(%d), use read_string instead",
            (int)count,
            (int)MAX_RET
        );
    }

    // 与 Lua string.byte 一致：提前检查栈空间
    luaL_checkstack(L, (int)count, "too many return values");

    for (lua_Integer i = 0; i < count; ++i) {
        lua_pushinteger(L, p[i]);
    }

    return (int)count;
}

// read_string(ptr, count) -> string (binary)
static int l_read_string(lua_State* L) {
    const char* p = (const char*)check_ptr(L, 1);
    lua_Integer count = luaL_checkinteger(L, 2);

    if (count <= 0) {
        lua_pushliteral(L, "");
        return 1;
    }

    // Lua 内部会自己 memcpy，这一步是“必要拷贝”
    lua_pushlstring(L, p, (size_t)count);
    return 1;
}

/* =========================================================
 * module
 * ========================================================= */

static const luaL_Reg unity_native_helper_funcs[] = {
    {"read_byte",      l_read_byte},
    {"write_byte",     l_write_byte},
    {"read_int32",     l_read_int32},
    {"write_int32",    l_write_int32},
    {"offset",       l_ptr_offset},

    // bulk
    {"read_bytes",   l_read_bytes},   // multi return
    {"read_string",  l_read_string},  // binary string

    {NULL, NULL}
};

LUALIB_API int luaopen_unity_native_helper(lua_State* L) {
    luaL_newlib(L, unity_native_helper_funcs);
    return 1;
}
