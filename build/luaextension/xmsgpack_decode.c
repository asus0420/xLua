/* Decode the MessagePack subset used by the launch index directly into Lua values.
 * The input is a Lua string; the result is allocated by the same Lua state.
 * Successful decoding returns the value and the next 1-based byte position.
 * Invalid or unsupported input returns nil so existing Lua callers can fall back.
 */
#include "lua.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    const unsigned char *cursor;
    const unsigned char *end;
} XMsgpackDecoder;

static size_t xmsgpack_remaining(const XMsgpackDecoder *decoder) {
    return (size_t)(decoder->end - decoder->cursor);
}

static int xmsgpack_read(XMsgpackDecoder *decoder, size_t count, uint64_t *value) {
    size_t i;
    uint64_t result = 0;
    if (xmsgpack_remaining(decoder) < count) return 0;
    for (i = 0; i < count; ++i) {
        result = (result << 8) | decoder->cursor[i];
    }
    decoder->cursor += count;
    *value = result;
    return 1;
}

/* Match Lua 5.3 string.unpack's integer result for the Lua fallback. */
static void xmsgpack_push_signed(lua_State *L, int64_t value) {
#if LUA_VERSION_NUM >= 503
    lua_pushinteger(L, (lua_Integer)value);
#else
    if (value >= INT32_MIN && value <= INT32_MAX) {
        lua_pushinteger(L, (lua_Integer)value);
    } else {
        lua_pushnumber(L, (lua_Number)value);
    }
#endif
}

static void xmsgpack_push_unsigned(lua_State *L, uint64_t value) {
#if LUA_VERSION_NUM >= 503
    lua_pushinteger(L, (lua_Integer)value);
#else
    if (value <= INT32_MAX) {
        lua_pushinteger(L, (lua_Integer)value);
    } else {
        lua_pushnumber(L, (lua_Number)value);
    }
#endif
}

static int xmsgpack_push_string(lua_State *L, XMsgpackDecoder *decoder, size_t count) {
    if (xmsgpack_remaining(decoder) < count) return 0;
    lua_pushlstring(L, (const char *)decoder->cursor, count);
    decoder->cursor += count;
    return 1;
}

static int xmsgpack_decode_value(lua_State *L, XMsgpackDecoder *decoder, unsigned int depth);

static int xmsgpack_decode_array(lua_State *L, XMsgpackDecoder *decoder,
                                uint64_t count, unsigned int depth) {
    uint64_t i;
    if (depth >= 100 || count > INT_MAX || count > xmsgpack_remaining(decoder)
        || !lua_checkstack(L, 4)) return 0;
    lua_createtable(L, (int)count, 0);
    for (i = 0; i < count; ++i) {
        if (!xmsgpack_decode_value(L, decoder, depth + 1)) return 0;
        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }
    return 1;
}

static int xmsgpack_decode_map(lua_State *L, XMsgpackDecoder *decoder,
                              uint64_t count, unsigned int depth) {
    uint64_t i;
    if (depth >= 100 || count > INT_MAX || count > xmsgpack_remaining(decoder) / 2
        || !lua_checkstack(L, 4)) return 0;
    lua_createtable(L, 0, (int)count);
    for (i = 0; i < count; ++i) {
        int key_type;
        if (!xmsgpack_decode_value(L, decoder, depth + 1)) return 0;
        key_type = lua_type(L, -1);
        if (key_type != LUA_TSTRING && key_type != LUA_TNUMBER && key_type != LUA_TBOOLEAN)
            return 0;
        if (key_type == LUA_TNUMBER) {
            lua_Number key = lua_tonumber(L, -1);
            if (key != key) return 0; /* NaN is not a valid Lua table key. */
        }
        if (!xmsgpack_decode_value(L, decoder, depth + 1)) return 0;
        lua_rawset(L, -3);
    }
    return 1;
}

static int xmsgpack_decode_value(lua_State *L, XMsgpackDecoder *decoder, unsigned int depth) {
    uint64_t tag, value;
    if (!xmsgpack_read(decoder, 1, &tag)) return 0;
    if (tag <= 0x7f) {
        xmsgpack_push_unsigned(L, tag);
        return 1;
    }
    if (tag >= 0xe0) {
        xmsgpack_push_signed(L, (int64_t)tag - 0x100);
        return 1;
    }
    if (tag >= 0xa0 && tag <= 0xbf)
        return xmsgpack_push_string(L, decoder, (size_t)(tag - 0xa0));
    if (tag >= 0x90 && tag <= 0x9f)
        return xmsgpack_decode_array(L, decoder, tag - 0x90, depth);
    if (tag >= 0x80 && tag <= 0x8f)
        return xmsgpack_decode_map(L, decoder, tag - 0x80, depth);

    switch (tag) {
        case 0xc0: lua_pushnil(L); return 1;
        case 0xc2: lua_pushboolean(L, 0); return 1;
        case 0xc3: lua_pushboolean(L, 1); return 1;
        case 0xc4: case 0xd9:
            return xmsgpack_read(decoder, 1, &value)
                && xmsgpack_push_string(L, decoder, (size_t)value);
        case 0xc5: case 0xda:
            return xmsgpack_read(decoder, 2, &value)
                && xmsgpack_push_string(L, decoder, (size_t)value);
        case 0xc6: case 0xdb:
            return xmsgpack_read(decoder, 4, &value)
                && xmsgpack_push_string(L, decoder, (size_t)value);
        case 0xca: {
            uint32_t bits;
            float number;
            if (!xmsgpack_read(decoder, 4, &value)) return 0;
            bits = (uint32_t)value;
            memcpy(&number, &bits, sizeof(number));
            lua_pushnumber(L, (lua_Number)number);
            return 1;
        }
        case 0xcb: {
            double number;
            if (!xmsgpack_read(decoder, 8, &value)) return 0;
            memcpy(&number, &value, sizeof(number));
            lua_pushnumber(L, (lua_Number)number);
            return 1;
        }
        case 0xcc: case 0xd0:
            if (!xmsgpack_read(decoder, 1, &value)) return 0;
            if (tag == 0xd0 && value >= 0x80) xmsgpack_push_signed(L, (int64_t)value - 0x100);
            else xmsgpack_push_unsigned(L, value);
            return 1;
        case 0xcd: case 0xd1:
            if (!xmsgpack_read(decoder, 2, &value)) return 0;
            if (tag == 0xd1 && value >= 0x8000) xmsgpack_push_signed(L, (int64_t)value - 0x10000);
            else xmsgpack_push_unsigned(L, value);
            return 1;
        case 0xce: case 0xd2:
            if (!xmsgpack_read(decoder, 4, &value)) return 0;
            if (tag == 0xd2 && value >= 0x80000000ULL)
                xmsgpack_push_signed(L, (int64_t)value - 0x100000000LL);
            else xmsgpack_push_unsigned(L, value);
            return 1;
        case 0xcf: case 0xd3:
            if (!xmsgpack_read(decoder, 8, &value)) return 0;
            if (tag == 0xd3) {
                int64_t signed_value;
                memcpy(&signed_value, &value, sizeof(signed_value));
                xmsgpack_push_signed(L, signed_value);
            } else {
                xmsgpack_push_unsigned(L, value);
            }
            return 1;
        case 0xdc: case 0xdd:
            return xmsgpack_read(decoder, tag == 0xdc ? 2 : 4, &value)
                && xmsgpack_decode_array(L, decoder, value, depth);
        case 0xde: case 0xdf:
            return xmsgpack_read(decoder, tag == 0xde ? 2 : 4, &value)
                && xmsgpack_decode_map(L, decoder, value, depth);
        default:
            return 0; /* 0xc1 and extension types match the C# fallback behavior. */
    }
}

int xlua_msgpack_decode(lua_State *L) {
    XMsgpackDecoder decoder;
    const char *input;
    size_t length;
    int initial_top = lua_gettop(L);
    input = lua_tolstring(L, 1, &length);
    if (input == NULL || length == 0) {
        lua_pushnil(L);
        return 1;
    }
    decoder.cursor = (const unsigned char *)input;
    decoder.end = decoder.cursor + length;
    if (!lua_isnoneornil(L, 2)) {
        lua_Number position = lua_tonumber(L, 2);
        if (!(position >= 1 && position <= (lua_Number)length)
            || position != (lua_Number)(size_t)position) {
            lua_pushnil(L);
            return 1;
        }
        decoder.cursor += (size_t)position - 1;
    }
    if (!xmsgpack_decode_value(L, &decoder, 0)) {
        lua_settop(L, initial_top);
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)(decoder.cursor - (const unsigned char *)input + 1));
    return 2;
}
