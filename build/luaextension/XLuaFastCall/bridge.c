#include "lauxlib.h"
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
#define INPUTS 8
#define OUTPUTS 4
/* Lua API 保证 C 函数至少有 LUA_MINSTACK 个可用槽，足够放结果和转换临时值。 */
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
   对象和 LuaTable 保留既有转换规则，支持范围由生成器决定。 */
enum
{
    T_INTEGER = 1,
    T_NUMBER = 2,
    T_OBJECT = 6,
    T_STRING = 7,
    T_TABLE = 8
};
extern int xlua_tocsobj_safe(lua_State *, int);
typedef struct
{
    union
    {
        int64_t i;
        double n;
        uint64_t address;
    };
    int64_t kind;
    union
    {
        float xyz[4];
        int64_t size;
    };
} Cell;
// 将 64 位句柄置于首位，统一 32 位编译器下的后续字段偏移。
typedef struct
{
    int64_t receiver;
    void *token;
    Cell *cells;
    char *error;
    int32_t count;
} Frame;
/* 独立于 Lua 内存布局的参数单元。标签由本模块定义，长度仅供字符串使用。 */
typedef struct
{
    union
    {
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
SD_ASSERT(cell_size, sizeof(Cell) == 32);
SD_ASSERT(cell_kind, offsetof(Cell, kind) == 8);
SD_ASSERT(cell_size_f, offsetof(Cell, size) == 16);
SD_ASSERT(frame_recv, offsetof(Frame, receiver) == 0);
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
    void *token;
    int closed, depth;
    char buffers[DEPTH][4096];
} Context;
typedef struct
{
    Callback callback;
    int count, required, allowExtra, types[INPUTS];
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
static int invoke(lua_State *L)
{
    Context *c = (Context *)lua_touserdata(L, lua_upvalueindex(1));
    Method *m = (Method *)lua_touserdata(L, lua_upvalueindex(2));
    int count = lua_gettop(L), i, status, d;
    int64_t receiver;
    Cell cells[INPUTS + OUTPUTS];
    Slot args[INPUTS];
    Frame frame;
    if (!c || c->closed)
        return luaL_error(L, "XLuaFastCall binding closed");
    if (c->depth >= DEPTH)
        return luaL_error(L, "XLuaFastCall nesting limit");
    if (count < m->required + 1 || (!m->allowExtra && count > m->count + 1))
        return luaL_error(L, "Wrong argument count");
    /* 句柄仍要求 Lua 整数，不能把浮点句柄自动窄化。 */
    {
        const TValue *h = L->ci->func + 1;
        if (!ttisinteger(h))
            return luaL_error(L, "Expected integer Proxy handle");
        receiver = (int64_t)ivalue(h);
    }
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
        else if (t == 3)
        {
            a->i = !l_isfalse(o);
            a->tag = S_BOOL;
        }
        else if (t == T_OBJECT)
        {
            if (ttisnil(o))
                cells[i].i = -1;
            else
            {
                cells[i].i = xlua_tocsobj_safe(L, index);
                if (cells[i].i < 0)
                    return luaL_error(L, "Expected managed object");
            }
        }
        else if (t == T_TABLE)
        {
            if (!ttistable(o))
                return luaL_error(L, "Expected LuaTable");
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
    d = c->depth++;
    frame.token = c->token;
    frame.cells = cells;
    frame.count = count;
    frame.receiver = receiver;
    frame.error = c->buffers[d];
    status = m->callback(L, args, &frame);
    /* 回调退出时统一恢复深度，错误文本保留到同深度的下一次调用。 */
    c->depth = d;

    if (status < 0)
        return luaL_error(L, "XLuaFastCall C# exception: %s", c->buffers[d]);
    if (status >= 100)
    {
        status -= 100;
        if (status > OUTPUTS || lua_gettop(L) != count + status)
            return luaL_error(L, "Return stack mismatch");
        return status;
    }
    if (status > OUTPUTS)
        return luaL_error(L, "Return stack mismatch");
    for (i = 0; i < status; i++)
    {
        Cell *v = cells + INPUTS + i;
        switch (v->kind)
        {
        case 1:
            lua_pushinteger(L, v->i);
            break;
        case 2:
            lua_pushnumber(L, v->n);
            break;
        case 3:
            lua_pushboolean(L, (int)v->i);
            break;
        case 6:
            if (v->address)
                lua_pushlstring(L, (const char *)(uintptr_t)v->address, (size_t)v->size);
            else
                lua_pushnil(L);
            break;
        default:
            return luaL_error(L, "Unknown return kind");
        }
    }
    return status;
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
    p[L_FRAME_ERROR] = offsetof(Frame, error);
    p[L_CELL_KIND] = offsetof(Cell, kind);
    p[L_CELL_SIZE] = offsetof(Cell, size);
    p[L_FRAME_TOKEN] = offsetof(Frame, token);
    p[L_FRAME] = sizeof(Frame);
    p[L_FRAME_RECEIVER] = offsetof(Frame, receiver);
    return 1002;
}

// 同一 Lua 注册表只允许创建一个上下文；正式入口没有验证参数。
API int sd_open(lua_State *L, void *token)
{
    Context *c;
    const uint16_t one = 1;
    if (*(const uint8_t *)&one != 1)
        return -1;
    if (*lua_version(L) != 503 || find(L))
        return -1;
    c = (Context *)lua_newuserdata(L, sizeof(Context));
    memset(c, 0, sizeof(*c));
    c->token = token;
    lua_rawsetp(L, LUA_REGISTRYINDEX, &key);
    return 0;
}
// 注册闭包，由上下文和方法描述两个 upvalue 保持生命周期。
API int sd_install(lua_State *L, const char *name, Callback callback, const int *types, int count,
                   int required, int allowExtra)
{
    Context *c = find(L);
    Method *m;
    int i;
    if (!c || c->closed || c->depth || !callback || count < 0 || count > INPUTS || required < 0 ||
        required > count || !lua_istable(L, -1))
        return -1;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &key);
    m = (Method *)lua_newuserdata(L, sizeof(Method));
    memset(m, 0, sizeof(*m));
    m->callback = callback;
    m->count = count;
    m->required = required;
    m->allowExtra = allowExtra;
    for (i = 0; i < count; i++)
    {
        int t = types[i];
        m->types[i] = t;
    }
    lua_pushcclosure(L, invoke, 2);
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
