#include "lua.h"
#include "lapi.h"
#include "lobject.h"
#include "lauxlib.h"
#include "ltable.h"
#include "lgc.h"
#include "lstate.h"
#include "lfunc.h"


#define MAX_RECURSION_DEPTH 256

static size_t calc_object_size(lua_State *L, int idx, int mark_tbl_idx, int depth);

static size_t calc_shallow_size(lua_State *L, int idx);

static void set_traversed(lua_State *L, const void *ptr, int mark_tbl_idx) {
    lua_pushlightuserdata(L, (void *) ptr);
    lua_pushboolean(L, 1);
    lua_rawset(L, mark_tbl_idx);
}

static int is_traversed(lua_State *L, const void *ptr, int mark_tbl_idx) {
    lua_pushlightuserdata(L, (void *) ptr);
    lua_rawget(L, mark_tbl_idx);
    int res = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return res;
}

static size_t get_string_size(const TString *s) {
    size_t size = sizeof(UTString)
                  + tsslen(s) + 1;
    return size;
}

static size_t get_table_size(const Table *t) {
    size_t size = sizeof(Table)
                  + sizeof(TValue) * t->sizearray
                  + sizeof(Node) * cast(size_t, allocsizenode(t));
    return size;
}

static size_t get_userdata_size(const Udata *ud) {
    size_t size = sizeof(UUdata)
                  + ud->len;
    return size;
}

static size_t get_thread_size(const lua_State *th) {
    size_t size = LUA_EXTRASPACE
                  + sizeof(lua_State)
                  + sizeof(TValue) * th->stacksize
                  + sizeof(CallInfo) * th->nci;
    return size;
}

static size_t get_lua_closure_size(const LClosure *cl) {
    size_t size = sizeof(LClosure);
    if (cl->nupvalues > 1) {
        size += sizeof(UpVal *) * (cl->nupvalues - 1);
    }
    return size;
}

static size_t get_c_closure_size(const CClosure *cl) {
    size_t size = sizeof(CClosure);
    if (cl->nupvalues > 1) {
        size += sizeof(TValue) * (cl->nupvalues - 1);
    }
    return size;
}

static size_t get_proto_size(const Proto *p) {
    size_t size = sizeof(Proto)
                  + sizeof(Instruction) * p->sizecode
                  + sizeof(Proto *) * p->sizep
                  + sizeof(TValue) * p->sizek
                  + sizeof(int) * p->sizelineinfo
                  + sizeof(LocVar) * p->sizelocvars
                  + sizeof(Upvaldesc) * p->sizeupvalues;
    return size;
}

static size_t calc_string_size(lua_State *L, int idx, int mark_tbl_idx, int depth) {
    const void *ptr = lua_tostring(L, idx);
    if (!ptr)
        return 0;

    const TString *s = (TString *) ((char *) ptr - sizeof(UTString));
    if (is_traversed(L, s, mark_tbl_idx))
        return 0;
    set_traversed(L, s, mark_tbl_idx);
    return get_string_size(s);
}

static size_t calc_table_size(lua_State *L, int idx, int mark_tbl_idx, int depth) {
    int abs_idx = lua_absindex(L, idx);
    Table *t = (Table *) lua_topointer(L, abs_idx);
    if (!t || is_traversed(L, t, mark_tbl_idx))
        return 0;

    set_traversed(L, t, mark_tbl_idx);
    size_t size = get_table_size(t);

    if (lua_getmetatable(L, abs_idx)) {
        size += calc_shallow_size(L, -1); // 元表只计算shallow size
        lua_pop(L, 1);
    }
    // 计算数组部分开销
    for (unsigned i = 1; i <= t->sizearray; ++i) {
        lua_rawgeti(L, abs_idx, i);
        if (!lua_isnil(L, -1)) {
            size += calc_object_size(L, -1, mark_tbl_idx, depth + 1);
        }
        lua_pop(L, 1);
    }
    // 计算hash部分开销
    lua_pushnil(L);
    while (lua_next(L, abs_idx)) {
        /* 此时：-2 = key, -1 = value */
        size += calc_object_size(L, -2, mark_tbl_idx, depth + 1);
        size += calc_object_size(L, -1, mark_tbl_idx, depth + 1);

        /* 弹出 value，保留 key 作为下一次迭代的 key */
        lua_pop(L, 1);
    }

    return size;
}

static size_t calc_userdata_size(lua_State *L, int idx, int mark_tbl_idx, int depth) {
    const void *ptr = lua_touserdata(L, idx);
    if (!ptr)
        return 0;
    Udata *ud = (Udata *) ((char *) ptr - sizeof(UUdata));
    if (is_traversed(L, ud, mark_tbl_idx))
        return 0;

    set_traversed(L, ud, mark_tbl_idx);
    size_t size = get_userdata_size(ud);

    if (lua_getmetatable(L, idx)) {
        size += calc_shallow_size(L, -1); // 元表只计算shallow size
        lua_pop(L, 1);
    }

    {
        int uv_type = lua_getuservalue(L, idx);
        if (uv_type != LUA_TNIL) {
            size += calc_object_size(L, -1, mark_tbl_idx, depth + 1);
        }
        lua_pop(L, 1); /* lua_getuservalue 即使返回 nil 也会压栈，必须无条件弹栈，避免破坏 lua_next 迭代栈 */
    }
    return size;
}

static size_t calc_thread_size(lua_State *L, int idx, int mark_tbl_idx) {
    lua_State *co = lua_tothread(L, idx);
    if (!co || is_traversed(L, co, mark_tbl_idx)) return 0;

    set_traversed(L, co, mark_tbl_idx);
    return get_thread_size(co);
}

static size_t calc_function_size(lua_State *L, int idx, int mark_tbl_idx, int depth) {
    idx = lua_absindex(L, idx);
    if (lua_iscfunction(L, idx)) {
        const char *upname = lua_getupvalue(L, idx, 1);
        if (upname == NULL) { /* 用 Lua C API 判断轻量 C 函数，避免依赖 index2addr 链接符号 */
            return 0;
        }
        lua_pop(L, 1); /* 命中 upvalue 时会压栈，需弹栈保持平衡 */
    }

    Closure *cl = (Closure *) lua_topointer(L, idx);
    if (!cl || is_traversed(L, cl, mark_tbl_idx))
        return 0;

    set_traversed(L, cl, mark_tbl_idx);
    size_t size = 0;
    if (cl->c.tt == LUA_TLCL) {
        const LClosure *lcl = &cl->l;

        size = get_lua_closure_size(lcl);

        if (lcl->p && !is_traversed(L, lcl->p, mark_tbl_idx)) {
            size += get_proto_size(lcl->p);
            set_traversed(L, lcl->p, mark_tbl_idx);
        }
    } else if (cl->c.tt == LUA_TCCL) {
        const CClosure *ccl = &cl->c;
        size = get_c_closure_size(ccl);
    } else
        size = sizeof(CClosure);

    return size;
}

static size_t calc_object_size(lua_State *L, int idx, int mark_tbl_idx, int depth) {
    if (depth > MAX_RECURSION_DEPTH)
        return 0;

    idx = lua_absindex(L, idx);

    switch (lua_type(L, idx)) {
        case LUA_TSTRING:
            return calc_string_size(L, idx, mark_tbl_idx, depth);
        case LUA_TTABLE:
            return calc_table_size(L, idx, mark_tbl_idx, depth);
        case LUA_TUSERDATA:
            return calc_userdata_size(L, idx, mark_tbl_idx, depth);
        case LUA_TTHREAD:
            return calc_thread_size(L, idx, mark_tbl_idx);
        case LUA_TFUNCTION:
            return calc_function_size(L, idx, mark_tbl_idx, depth);
        default:
            return 0;
    }
}

static size_t calc_shallow_size(lua_State *L, int idx) {
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
        case LUA_TSTRING: {
            const void *ptr = lua_tostring(L, idx);
            if (!ptr) return 0;
            TString *s = (TString *) ((char *) ptr - sizeof(UTString));
            return get_string_size(s);
        }
        case LUA_TTABLE: {
            Table *t = (Table *) lua_topointer(L, idx);
            if (!t) return 0;
            return get_table_size(t);
        }
        case LUA_TUSERDATA: {
            const void *ptr = lua_touserdata(L, idx);
            if (!ptr) return 0;
            Udata *ud = (Udata *) ((char *) ptr - sizeof(UUdata));
            return get_userdata_size(ud);
        }
        case LUA_TTHREAD: {
            lua_State *co = lua_tothread(L, idx);
            if (!co) return 0;
            return get_thread_size(co);
        }
        case LUA_TFUNCTION: {
            if (lua_iscfunction(L, idx)) {
                const char *upname = lua_getupvalue(L, idx, 1);
                if (upname == NULL) return 0; /* 轻量 C 函数无闭包内存，返回 0 */
                lua_pop(L, 1); /* 命中 upvalue 时会压栈，需弹栈保持平衡 */
            }
            Closure *cl = (Closure *) lua_topointer(L, idx);
            if (!cl) return 0;
            if (cl->c.tt == LUA_TLCL)
                return get_lua_closure_size(&cl->l);
            if (cl->c.tt == LUA_TCCL)
                return get_c_closure_size(&cl->c);
            return sizeof(CClosure);
        }
        default:
            return 0;
    }
}

static int l_get_total_size(lua_State *L) {
    int top = lua_gettop(L);
    int mark_tbl_idx = 0;
    int created_mark_tbl = 0;
    if (lua_istable(L, 2)) {
        mark_tbl_idx = lua_absindex(L, 2);
    } else {
        lua_newtable(L);
        mark_tbl_idx = lua_gettop(L);
        created_mark_tbl = 1;
    }
    // 获取对象的下标
    int target_idx = lua_absindex(L, 1);

    const size_t size = calc_object_size(L, target_idx, mark_tbl_idx, 0);

    if (created_mark_tbl) {
        lua_settop(L, top);
    }

    lua_pushinteger(L, (lua_Integer) size);
    return 1;
}

static int l_get_shallow_size(lua_State *L) {
    const size_t size = calc_shallow_size(L, 1);
    lua_pushinteger(L, (lua_Integer) size);
    return 1;
}

static const luaL_Reg lua_mem_util_funcs[] = {
    {"GetTotalSize", l_get_total_size}, /* 总体占用 */
    {"GetSelfSize", l_get_shallow_size}, /* 自身占用（浅表） */
    {NULL, NULL}
};

LUALIB_API int luaopen_lua_memory_tool(lua_State *L) {
    luaL_newlib(L, lua_mem_util_funcs);
    return 1;
}
