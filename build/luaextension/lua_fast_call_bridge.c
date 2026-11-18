#include "lauxlib.h"
#include "ldo.h"
#include "lobject.h"
#include "lstate.h"
#include "lua.h"
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#if defined(_WIN32)
#define API __declspec(dllexport)
#define SD_CALL __cdecl
#else
#define API __attribute__((visibility("default")))
#define SD_CALL
#endif
#define DEPTH 32
#define INPUTS 32
#define OUTPUTS 16
/* Lua API 保证 C 函数至少有 LUA_MINSTACK 个可用槽，足够放结果和转换临时值。 */
/* 16 + 4 已到 LUA_MINSTACK 边界；再次扩容须在 invoke 中显式 luaL_checkstack。 */
typedef char outputs_fit_min_stack[(OUTPUTS + 4 <= LUA_MINSTACK) ? 1 : -1];
/* 自定义 ABI 的检查项；编号与托管端一致。 */
enum
{
    L_SLOT,
    L_SLOT_TAG,
    L_CELL,
    L_SLOT_LENGTH,
    L_FRAME_CELLS,
    L_FRAME_COUNT,
    L_FRAME_ERROR,
    L_CELL_KIND,
    L_CELL_SIZE,
    L_FRAME_TOKEN,
    L_FRAME,
    L_FRAME_RECEIVER,
    L_COUNT
};
/* 参数类型来自生成器。常见输入通过内部宏读取，兼容转换回退到公开 API。
   仅支持生成器声明的标量输入。 */
enum
{
    T_INTEGER = 1,
    T_NUMBER = 2,
    T_BOOL = 3,
    T_STRING = 7,
    T_STRICT_NUMBER = 10
};
enum
{
    R_INTEGER = 1,
    R_NUMBER = 2,
    R_BOOL = 3,
    R_STRING = 6
};
typedef struct
{
    union {
        int64_t i;
        double n;
        uint64_t address;
    };
    int64_t kind;
    int64_t size;
} Cell;
// 将 64 位句柄置于首位，统一 32 位编译器下的后续字段偏移。
typedef struct
{
    int64_t receiver;
    Cell *cells;
    char *error;
    int32_t count;
    /* 显式补齐，避免 i386 GCC 的 int64 对齐为 4、MSVC/C# 为 8 时大小不同。 */
    int32_t reserved;
} Frame;
/* 独立于 Lua 内存布局的参数单元。标签由本模块定义，长度仅供字符串使用。 */
typedef struct
{
    union {
        int64_t i;
        double n;
        uint64_t address;
    };
    int32_t tag;
    int32_t length;
} Slot;
#define SD_ASSERT(name, c) typedef char sd_assert_##name[(c) ? 1 : -1]
SD_ASSERT(slot_size, sizeof(Slot) == 16);
SD_ASSERT(slot_tag, offsetof(Slot, tag) == 8);
SD_ASSERT(slot_length, offsetof(Slot, length) == 12);
SD_ASSERT(cell_size, sizeof(Cell) == 24);
SD_ASSERT(cell_kind, offsetof(Cell, kind) == 8);
SD_ASSERT(cell_size_f, offsetof(Cell, size) == 16);
SD_ASSERT(frame_recv, offsetof(Frame, receiver) == 0);
SD_ASSERT(frame_size, sizeof(Frame) == (sizeof(void *) == 8 ? 32 : 24));
enum
{
    S_NIL = 0,
    S_BOOL = 1,
    S_NUMBER = 3,
    S_STRING = 4,
    S_INTEGER = 19
};
typedef int(SD_CALL *Callback)(lua_State *, const Slot *, Frame *);
typedef struct
{
    int closed, depth;
    char buffers[DEPTH][4096];
} Context;
typedef struct
{
    Callback callback;
    int count, required, strict, allowExtra, userdataOnly, types[INPUTS];
} Method;
static char key;
static Context *find(lua_State *L)
{
    Context *c;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &key);
    c = (Context *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return c;
}
/* 整数入口保持兼容；userdata 只在首次调用读取托管属性。
   uservalue 专用于句柄缓存，不能覆盖其他库的非整数值。
   lua_getfield 可能重入/扩栈，调用后不得继续使用先前的 TValue 指针。 */
static int receiver_of(lua_State *L, int64_t *out)
{
    const TValue *h = L->ci->func + 1;
    TValue v;
    if (ttisinteger(h))
    {
        *out = (int64_t)ivalue(h);
        return 1;
    }
    if (!ttisfulluserdata(h))
        return 0;
    getuservalue(L, uvalue(h), &v);
    if (ttisinteger(&v))
    {
        *out = (int64_t)ivalue(&v);
        return 1;
    }
    if (!ttisnil(&v))
        return luaL_error(L, "XLuaFastCall Proxy uservalue is already in use");
    lua_getfield(L, 1, lua_tostring(L, lua_upvalueindex(3)));
    if (lua_isinteger(L, -1) && lua_tointeger(L, -1) > 0)
    {
        *out = (int64_t)lua_tointeger(L, -1);
        /* getter 可能重入并改变 uservalue，不覆盖其他用途的数据。 */
        h = L->ci->func + 1;
        getuservalue(L, uvalue(h), &v);
        if (!ttisnil(&v) && (!ttisinteger(&v) || ivalue(&v) != *out))
        {
            lua_pop(L, 1);
            return luaL_error(L, "XLuaFastCall Proxy uservalue changed during handle lookup");
        }
        lua_setuservalue(L, 1);
        return 1;
    }
    lua_pop(L, 1);
    return 0;
}

/* 对象池重新注册句柄时调用；不调用 metamethod，不跨 P/Invoke 抛 Lua 错误。 */
API int sd_reset_receiver(lua_State *L, int index)
{
    int kind;
    if (lua_type(L, index) != LUA_TUSERDATA)
        return -1;
    kind = lua_getuservalue(L, index);
    if (kind != LUA_TNIL && !lua_isinteger(L, -1))
    {
        lua_pop(L, 1);
        return -1;
    }
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_setuservalue(L, index < 0 ? index - 1 : index);
    return 0;
}

/* Only allocating Lua pushes need protection. Catch inside native code so
   Lua errors cannot jump across a managed callback/PInvoke frame. */
typedef struct
{
    const char *text;
    int size;
} StringPush;

static void push_string(lua_State *L, void *data)
{
    StringPush *s = (StringPush *)data;
    /* Validate within the protected call: failure must also leave an error object. */
    if (s->size < 0 || (!s->text && s->size))
        luaL_error(L, "XLuaFastCall invalid string buffer");
    lua_pushlstring(L, s->text, (size_t)s->size);
}

API int sd_pushlstring(lua_State *L, const char *text, int size)
{
    StringPush data;
    data.text = text;
    data.size = size;
    return luaD_pcall(L, push_string, &data, savestack(L, L->top), 0);
}

static void push_exception(lua_State *L, void *data)
{
    /* Preserve the Lua caller's file/line; copy before releasing this depth. */
    luaL_where(L, 1);
    lua_pushfstring(L, "XLuaFastCall C# exception: %s", (const char *)data);
    lua_concat(L, 2);
}

static int invoke_method(lua_State *L, Method *m)
{
    Context *c = (Context *)lua_touserdata(L, lua_upvalueindex(1));
    int count = lua_gettop(L), i, status, d;
    int64_t receiver;
    Cell cells[INPUTS + OUTPUTS];
    Slot args[INPUTS];
    Frame frame;
    if (!c || c->closed)
        return luaL_error(L, "XLuaFastCall binding closed");
    if (c->depth >= DEPTH)
        return luaL_error(L, "XLuaFastCall nesting limit");
    if (m->strict)
    {
        int argc = count - 1;
        if (argc < m->required || argc > m->count)
            goto invalid;
        /* 只读 TValue，不调用可能重入或扩栈的 API。先检查参数，再查 receiver。 */
        for (i = 0; i < argc; i++)
        {
            const TValue *o = L->ci->func + i + 2;
            int t = m->types[i];
            if (t == T_STRING)
            {
                if (!ttisnil(o) && !ttisstring(o)) goto invalid;
            }
            else if (t == T_BOOL)
            {
                if (!ttisboolean(o)) goto invalid;
            }
            else if (!ttisnumber(o)) goto invalid;
        }
    }
    else if (count < m->required + 1 || (!m->allowExtra && count > m->count + 1))
        return luaL_error(L, "Wrong argument count");
    if (m->userdataOnly && (count < 1 || !ttisfulluserdata(L->ci->func + 1)))
        return luaL_error(L, "Expected Proxy userdata as self");
    if (!receiver_of(L, &receiver))
        return luaL_error(L, "Expected integer Proxy handle or Proxy userdata with a valid handle");
    /* 首次属性 getter 可以回调 Lua/C#，返回后重新确认绑定仍然有效。 */
    if (c->closed)
        return luaL_error(L, "XLuaFastCall binding closed during handle lookup");
    for (i = 0; i < m->count && i + 1 < count; i++)
    {
        int index = i + 2, t = m->types[i];
        /* 上一轮的转换可能触发 GC 和栈重分配，必须从当前调用帧重新获取地址。 */
        const TValue *o = L->ci->func + index;
        Slot *a = args + i;
        a->tag = S_NIL;
        if (t == T_STRING)
        {
            size_t length = 0;
            const char *text;
            if (ttisstring(o))
            {
                TString *s = tsvalue(o);
                text = getstr(s);
                length = tsslen(s);
            }
            else
            {
                /* 数字仍原地转字符串；调用后不再访问此前的 o。 */
                text = lua_tolstring(L, index, &length);
            }
            if (length > INT_MAX)
                return luaL_error(L, "String argument too large");
            a->address = (uint64_t)(uintptr_t)text;
            a->length = (int32_t)length;
            a->tag = S_STRING;
        }
        else if (t == T_BOOL)
        {
            a->i = !l_isfalse(o);
            a->tag = S_BOOL;
        }
        else if (ttisinteger(o))
        {
            a->i = (int64_t)ivalue(o);
            a->tag = S_INTEGER;
        }
        else if (ttisfloat(o))
        {
            a->n = (double)fltvalue(o);
            a->tag = S_NUMBER;
        }
        else
        {
            /* 保持旧转换行为：普通数字允许数字字符串，严格数字由回调拒绝。 */
            if (t == T_INTEGER)
                cells[i].i = lua_tointeger(L, index);
            if (t == T_NUMBER)
                cells[i].n = lua_tonumber(L, index);
        }
    }
    /* 未传入的可选槽不读取 Lua 栈；初始化以防止未定义值泄漏。 */
    for (; i < m->count; i++)
    {
        memset(args + i, 0, sizeof(Slot));
        memset(cells + i, 0, sizeof(Cell));
    }
    /* Argument conversion can run __gc and close the binding. */
    if (c->closed)
        return luaL_error(L, "XLuaFastCall binding closed during argument conversion");
    d = c->depth++;
    frame.cells = cells;
    frame.receiver = receiver;
    frame.error = c->buffers[d];
    frame.count = count;
    frame.reserved = 0;
    status = m->callback(L, args, &frame);
    /* Keep this buffer reserved through all result pushes and error formatting.
       Each allocating push is protected natively, so every Lua error restores depth. */
    if (status < 0)
    {
        int error;
        if (status < -1)
        {
            c->depth = d;
            /* Only these encoded failures carry a Lua error object. Reject yield,
               unknown codes and INT_MIN before decoding the status. */
            switch (status)
            {
            case -1 - LUA_ERRRUN:
            case -1 - LUA_ERRMEM:
            case -1 - LUA_ERRGCMM:
            case -1 - LUA_ERRERR:
                luaD_throw(L, -1 - status);
                break;
            default:
                return luaL_error(L, "XLuaFastCall invalid callback error status");
            }
        }
        error = luaD_pcall(L, push_exception, frame.error, savestack(L, L->top), 0);
        c->depth = d;
        if (error != LUA_OK)
            luaD_throw(L, error);
        return lua_error(L);
    }
    if (status >= 100)
    {
        status -= 100;
        c->depth = d;
        if (status > OUTPUTS || lua_gettop(L) != count + status)
            return luaL_error(L, "Return stack mismatch");
        return status;
    }
    if (status > OUTPUTS)
    {
        c->depth = d;
        return luaL_error(L, "Return stack mismatch");
    }
    for (i = 0; i < status; i++)
    {
        Cell *v = cells + INPUTS + i;
        switch (v->kind)
        {
        case R_INTEGER:
            lua_pushinteger(L, v->i);
            break;
        case R_NUMBER:
            lua_pushnumber(L, v->n);
            break;
        case R_BOOL:
            lua_pushboolean(L, (int)v->i);
            break;
        case R_STRING:
            if (v->address)
            {
                int error = sd_pushlstring(L, (const char *)(uintptr_t)v->address, (int)v->size);
                if (error != LUA_OK)
                {
                    c->depth = d;
                    luaD_throw(L, error);
                }
            }
            else
                lua_pushnil(L);
            break;
        default:
            c->depth = d;
            return luaL_error(L, "Unknown return kind");
        }
    }
    c->depth = d;
    return status;
invalid:
    /* xLua C# luaL_error 只压入消息，不附带 luaL_where 文件/行号。 */
    lua_pushvalue(L, lua_upvalueindex(4));
    return lua_error(L);
}

/* Single methods keep their own entry; only group entries scan overloads. */
static int invoke(lua_State *L) {
  return invoke_method(L, (Method *)lua_touserdata(L, lua_upvalueindex(2)));
}
typedef struct { int count; Method methods[1]; } MethodGroup;
static int invoke_group(lua_State *L) {
  MethodGroup *group = (MethodGroup *)lua_touserdata(L, lua_upvalueindex(2));
  int argc = lua_gettop(L) - 1, branch, i;
  for (branch = 0; branch < group->count; branch++) {
    Method *m = &group->methods[branch];
    if (argc != m->required) continue;
    for (i = 0; i < argc; i++) {
      const TValue *v = L->ci->func + i + 2;
      int kind = m->types[i];
      if (kind == T_STRING) { if (!ttisnil(v) && !ttisstring(v)) break; }
      else if (kind == T_BOOL) { if (!ttisboolean(v)) break; }
      else if (!ttisnumber(v)) break;
    }
    if (i == argc) return invoke_method(L, m);
  }
  lua_pushvalue(L, lua_upvalueindex(4));
  return lua_error(L);
}

// 报告自定义结构布局，不再暴露 Lua 私有结构偏移。
API int sd_layout(int *p)
{
    /* 自定义 ABI 版本；旧 DLL 不会通过托管端校验。 */
    memset(p, 0, L_COUNT * sizeof(int));
    p[L_SLOT] = sizeof(Slot);
    p[L_SLOT_TAG] = offsetof(Slot, tag);
    p[L_CELL] = sizeof(Cell);
    p[L_SLOT_LENGTH] = offsetof(Slot, length);
    p[L_FRAME_CELLS] = offsetof(Frame, cells);
    p[L_FRAME_COUNT] = offsetof(Frame, count);
    /* Token 槽继续保留为零；sd_layout 始终只写 12 项。 */
    p[L_FRAME_ERROR] = offsetof(Frame, error);
    p[L_CELL_KIND] = offsetof(Cell, kind);
    p[L_CELL_SIZE] = offsetof(Cell, size);
    p[L_FRAME] = sizeof(Frame);
    p[L_FRAME_RECEIVER] = offsetof(Frame, receiver);
    return 1009;
}

// 同一 Lua 注册表只允许创建一个上下文；正式入口没有验证参数。
API int sd_open(lua_State *L)
{
    Context *c;
    const uint16_t one = 1;
    if (*(const uint8_t *)&one != 1)
        return -1;
    if (*lua_version(L) != 503 || find(L))
        return -1;
    c = (Context *)lua_newuserdata(L, sizeof(Context));
    memset(c, 0, sizeof(*c));
    lua_rawsetp(L, LUA_REGISTRYINDEX, &key);
    return 0;
}
// 注册闭包：上下文、方法描述、句柄属性名、严格模式错误字符串四个 upvalue。
static int install(lua_State *L, const char *name, Callback callback, const int *types, int count,
                   int required, int allowExtra, int userdataOnly, const char *handleField, const char *strictName)
{
    Context *c = find(L);
    Method *m;
    int i;
    if (!c || c->closed || c->depth || !name || !callback || !handleField || !handleField[0] || count < 0 ||
        count > INPUTS || required < 0 || required > count || (strictName && allowExtra) ||
        (allowExtra != 0 && allowExtra != 1) ||
        (count && !types) || !lua_istable(L, -1))
        return -1;
    for (i = 0; i < count; i++)
        if (types[i] != T_INTEGER && types[i] != T_BOOL && types[i] != T_STRING &&
            types[i] != T_STRICT_NUMBER && (types[i] != T_NUMBER || !userdataOnly))
            return -1;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &key);
    m = (Method *)lua_newuserdata(L, sizeof(Method));
    memset(m, 0, sizeof(*m));
    m->callback = callback;
    m->count = count;
    m->required = required;
    m->strict = strictName != NULL;
    m->allowExtra = allowExtra;
    m->userdataOnly = userdataOnly;
    for (i = 0; i < count; i++)
    {
        int t = types[i];
        m->types[i] = t;
    }
    lua_pushstring(L, handleField);
    if (strictName) lua_pushfstring(L, "invalid arguments to %s!", strictName);
    else lua_pushnil(L);
    lua_pushcclosure(L, invoke, 4);
    lua_setfield(L, -2, name);
    return 0;
}
// Preserve the existing production export and its exact-count legacy contract.
API int sd_install(lua_State *L, const char *name, Callback callback, const int *types, int count,
                   int required, const char *handleField, const char *strictName)
{
    if (!strictName && required != count) return -1;
    return install(L, name, callback, types, count, required, 0, 0, handleField, strictName);
}
API int sd_install_proxy(lua_State *L, const char *name, Callback callback, const int *types, int count,
                         int required, int allowExtra, const char *handleField, const char *strictName)
{
    return install(L, name, callback, types, count, required, allowExtra, 1, handleField, strictName);
}
/* All descriptors are copied at registration; no per-call allocation. */
API int sd_install_group(lua_State *L, const char *name, const Callback *callbacks,
                         const int *counts, const int *arities, const int *types,
                         int branches, const char *handleField, const char *strictName) {
  Context *c = find(L);
  MethodGroup *group;
  int b, i;
  if (!c || c->closed || c->depth || !name || !callbacks || !counts || !arities || !types ||
      !handleField || !handleField[0] || !strictName || branches < 2 || branches > 1024 || !lua_istable(L, -1)) return -1;
  for (b = 0; b < branches; b++) {
    if (!callbacks[b] || counts[b] < 0 || counts[b] > INPUTS || arities[b] < 0 || arities[b] > counts[b]) return -1;
    for (i = 0; i < counts[b]; i++) {
      int t = types[b * INPUTS + i];
      if (t != T_INTEGER && t != T_STRICT_NUMBER && t != T_BOOL && t != T_STRING) return -1;
    }
  }
  lua_rawgetp(L, LUA_REGISTRYINDEX, &key);
  group = (MethodGroup *)lua_newuserdata(L, offsetof(MethodGroup, methods) + branches * sizeof(Method));
  group->count = branches;
  for (b = 0; b < branches; b++) {
    Method *m = &group->methods[b];
    memset(m, 0, sizeof(*m));
    m->callback = callbacks[b]; m->count = counts[b]; m->required = arities[b]; m->userdataOnly = 1;
    for (i = 0; i < counts[b]; i++) m->types[i] = types[b * INPUTS + i];
  }
  lua_pushstring(L, handleField);
  lua_pushfstring(L, "invalid arguments to %s!", strictName);
  lua_pushcclosure(L, invoke_group, 4);
  lua_setfield(L, -2, name);
  return 0;
}

// 关闭后保留上下文供旧闭包检查，回调期间拒绝关闭。
API int sd_close(lua_State *L)
{
    Context *c = find(L);
    if (!c || c->closed)
        return 0;
    if (c->depth)
        return -1;
    c->closed = 1;
    return 0;
}

/* LuaTableWriter: independent of the FastCall protocol. Lua 5.3 main state only. */
#include "lstring.h"
#include "lgc.h"
#define TW_OK 0
#define TW_LUAERR 1
#define TW_STALE -1
#define TW_NOSTACK -2
#define TW_STATE -3
#define TW_BADARGS -4
#define TW_NO_CALLBACK -5
static int tw_valid(lua_State *L, int idx, const void *ptr) {
    return L && ptr && idx > 0 && idx <= lua_gettop(L) &&
        lua_type(L, idx) == LUA_TTABLE && lua_topointer(L, idx) == ptr;
}
API int xlua_tw_main(lua_State *L) { return L && L == G(L)->mainthread; }
typedef struct { int array, hash; } tw_new_args;
static void tw_do_new(lua_State *L, void *ud) {
    tw_new_args *a = (tw_new_args*)ud;
    lua_createtable(L, a->array, a->hash);
}
API int xlua_tw_new(lua_State *L, int parent, const void *parent_ptr,
                    int array, int hash, int *idx, const void **ptr) {
    tw_new_args a;
    if (!L || L != G(L)->mainthread) return TW_STATE;
    /* Auxiliary misuse guard, not a lifetime token or proof of a Writer wrap. */
    if (L->ci == &L->base_ci || isLua(L->ci)) return TW_NO_CALLBACK;
    if (parent && !tw_valid(L, parent, parent_ptr)) return TW_STALE;
    if (array < 0 || hash < 0) return TW_BADARGS;
    if (!lua_checkstack(L, 3)) return TW_NOSTACK;
    a.array = array; a.hash = hash;
    if (luaD_pcall(L, tw_do_new, &a, savestack(L, L->top), 0) != LUA_OK) return TW_LUAERR;
    *idx = lua_gettop(L); *ptr = lua_topointer(L, -1);
    return TW_OK;
}
/* kind: 0=nil, 1=integer, 2=number, 3=boolean, 4=UTF8, 5=table, 6=borrowed stack-top value.
   key_kind: 0=integer, 1=UTF8. Strings share a contiguous key/value buffer. */
typedef struct {
    int idx, key_kind, key_len, value_len, kind, child, value_slot;
    lua_Integer key, integer;
    lua_Number number;
    const char *bytes;
} tw_set_args;
static void tw_push_string(lua_State *L, const char *bytes, int len) {
    TString *s = luaS_newlstr(L, bytes, (size_t)len);
    setsvalue2s(L, L->top, s); L->top++;
}
static void tw_do_set(lua_State *L, void *ud) {
    tw_set_args *a = (tw_set_args*)ud;
    if (a->key_kind) {
        /* Reject nil before calculating a value pointer from key_len. */
        if (a->key_len < 0) luaL_error(L, "table index is nil");
        tw_push_string(L, a->bytes, a->key_len);
    } else lua_pushinteger(L, a->key);
    switch (a->kind) {
        case 0: lua_pushnil(L); break;
        case 1: lua_pushinteger(L, a->integer); break;
        case 2: lua_pushnumber(L, a->number); break;
        case 3: lua_pushboolean(L, a->integer != 0); break;
        case 4: tw_push_string(L, a->bytes + (a->key_kind ? a->key_len : 0), a->value_len); break;
        case 5: lua_pushvalue(L, a->child); break;
        case 6: lua_pushvalue(L, a->value_slot); break;
    }
    lua_rawset(L, a->idx);
    /* Both strings have been copied/rooted before finalizers can reenter C#. */
    if (a->key_kind || a->kind == 4) luaC_checkGC(L);
}
API int xlua_tw_set(lua_State *L, int idx, const void *ptr,
    int key_kind, lua_Integer key, const char *bytes, int bytes_len, int key_len,
    int kind, lua_Integer integer, lua_Number number, int value_len,
    lua_State *child_owner, int child, const void *child_ptr) {
    tw_set_args a;
    if (!tw_valid(L, idx, ptr)) return TW_STALE;
    if (key_kind < 0 || key_kind > 1 || kind < 0 || kind > 6 || key_len < -1 || value_len < 0) return TW_BADARGS;
    if ((key_kind && key_len > 0) || kind == 4) {
        int prefix = key_kind && key_len > 0 ? key_len : 0;
        if (!bytes || bytes_len < prefix || value_len > bytes_len - prefix) return TW_BADARGS;
    }
    if (kind == 5) {
        if (L != child_owner) return TW_STATE;
        if (!tw_valid(L, child, child_ptr)) return TW_STALE;
    }
    /* Borrow, do not consume: caller restores its entry top on every return. */
    a.value_slot = kind == 6 ? lua_gettop(L) : 0;
    if (kind == 6 && a.value_slot <= idx) return TW_BADARGS;
    if (!lua_checkstack(L, 2)) return TW_NOSTACK;
    a.idx=idx; a.key_kind=key_kind; a.key=key; a.bytes=bytes;
    a.key_len=key_len; a.kind=kind; a.integer=integer; a.number=number;
    a.value_len=value_len; a.child=child;
    return luaD_pcall(L, tw_do_set, &a, savestack(L, L->top), 0) == LUA_OK ? TW_OK : TW_LUAERR;
}
API int xlua_tw_push(lua_State *L, lua_State *owner, int idx, const void *ptr) {
    if (L != owner) return TW_STATE;
    if (!tw_valid(L, idx, ptr)) return TW_STALE;
    if (!lua_checkstack(L, 1)) return TW_NOSTACK;
    lua_pushvalue(L, idx);
    return TW_OK;
}
