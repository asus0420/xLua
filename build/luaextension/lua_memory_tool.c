#include "lua.h"
#include "lauxlib.h"

#if LUA_VERSION_NUM == 503

#include <string.h>
#include <stdint.h>
#include "lapi.h"
#include "lobject.h"
#include "ltable.h"
#include "lstate.h"
#include "lstring.h"
#include "lfunc.h"

#define MARK_NONE     0
#define MARK_SHALLOW  1
#define MARK_DEEP     2
#define MARK_EXTERNAL 3

#define PTR_SET_META "lmt.PtrSet"

typedef struct {
    uintptr_t *entries;  /* ptr | mark（低 2 位），0 = 空槽 */
    size_t capacity;     /* 2 的幂 */
    size_t count;
} PtrSet;

static void *ptr_set_realloc(lua_State *L, void *block, size_t osize, size_t nsize) {
    void *ud;
    lua_Alloc allocf = lua_getallocf(L, &ud);
    return allocf(ud, block, osize, nsize);
}

static size_t ptr_set_start_slot(const PtrSet *s, uintptr_t key) {
    /* 指针低位因对齐无熵，先右移再乘 Knuth 常数搅拌 */
    return (size_t) ((key >> 3) * 2654435761u) & (s->capacity - 1);
}

static int ptr_set_get(const PtrSet *s, const void *ptr) {
    uintptr_t key = (uintptr_t) ptr;
    size_t i;
    if (s->capacity == 0)
        return MARK_NONE;
    i = ptr_set_start_slot(s, key);
    while (s->entries[i] != 0) {
        if ((s->entries[i] & ~(uintptr_t) 3) == key)
            return (int) (s->entries[i] & 3);
        i = (i + 1) & (s->capacity - 1);
    }
    return MARK_NONE;
}

static void ptr_set_grow(lua_State *L, PtrSet *s) {
    size_t old_cap = s->capacity;
    uintptr_t *old_entries = s->entries;
    size_t ncap = old_cap ? old_cap * 2 : 4096;
    size_t i;
    uintptr_t *ne = (uintptr_t *) ptr_set_realloc(L, NULL, 0, ncap * sizeof(uintptr_t));
    if (ne == NULL) /* 未动 s，旧 entries 仍由 __gc 兜底 */
        luaL_error(L, "lua_memory_tool: mark set out of memory");
    memset(ne, 0, ncap * sizeof(uintptr_t));
    s->entries = ne;
    s->capacity = ncap;
    for (i = 0; i < old_cap; ++i) {
        uintptr_t e = old_entries[i];
        if (e != 0) {
            size_t j = ptr_set_start_slot(s, e & ~(uintptr_t) 3);
            while (ne[j] != 0)
                j = (j + 1) & (ncap - 1);
            ne[j] = e;
        }
    }
    if (old_entries)
        ptr_set_realloc(L, old_entries, old_cap * sizeof(uintptr_t), 0);
}

static void ptr_set_set(lua_State *L, PtrSet *s, const void *ptr, int level) {
    uintptr_t key = (uintptr_t) ptr; /* GC 对象由分配器返回，低 2 位必为 0 */
    size_t i;
    if (s->count * 4 >= s->capacity * 3) /* 负载因子上限 3/4 */
        ptr_set_grow(L, s);
    i = ptr_set_start_slot(s, key);
    while (s->entries[i] != 0) {
        if ((s->entries[i] & ~(uintptr_t) 3) == key) {
            s->entries[i] = key | (uintptr_t) level; /* mark 升级（SHALLOW -> DEEP） */
            return;
        }
        i = (i + 1) & (s->capacity - 1);
    }
    s->entries[i] = key | (uintptr_t) level;
    s->count++;
}

static void ptr_set_dispose(lua_State *L, PtrSet *s) {
    if (s->entries) {
        ptr_set_realloc(L, s->entries, s->capacity * sizeof(uintptr_t), 0);
        s->entries = NULL;
        s->capacity = 0;
        s->count = 0;
    }
}

static int ptr_set_gc(lua_State *L) {
    ptr_set_dispose(L, (PtrSet *) lua_touserdata(L, 1));
    return 0;
}

static PtrSet *ptr_set_push_new(lua_State *L) {
    PtrSet *s = (PtrSet *) lua_newuserdata(L, sizeof(PtrSet));
    s->entries = NULL;
    s->capacity = 0;
    s->count = 0;
    if (luaL_newmetatable(L, PTR_SET_META)) {
        lua_pushcfunction(L, ptr_set_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
    return s;
}

typedef struct {
    lua_State *L;
    PtrSet *set;           /* mark 存储：对象指针 -> MARK_* */
    int queue_idx;         /* 待展开队列（Lua 表）在栈上的绝对下标 */
    lua_Integer queue_n;   /* 队列元素个数 */
    size_t total;
    int deep_mt;           /* 元表是否深遍历 */
    int pass1;             /* retained pass1：只标记 MARK_EXTERNAL，不计大小 */
    const void *target_ptr;  /* retained pass1 的边界：目标对象指针，遇到即跳过 */
    const void *queue_ptr; /* 队列表指针：遍历中会向其写入，遇到时必须跳过 */
    int eph_idx;           /* 挂起的 weak-key 表列表（Lua 表）在栈上的绝对下标 */
    lua_Integer eph_n;     /* 列表元素个数 */
    const void *eph_ptr;   /* 列表表指针：同 queue_ptr，遇到时跳过 */
} MemCtx;

static int ctx_mark_get(MemCtx *ctx, const void *ptr) {
    return ptr_set_get(ctx->set, ptr);
}

static void ctx_mark_set(MemCtx *ctx, const void *ptr, int level) {
    ptr_set_set(ctx->L, ctx->set, ptr, level);
}

/* pass1 只标记不计数；visited_mark 决定对象被记为 EXTERNAL 还是 DEEP */
static void add_size(MemCtx *ctx, size_t size) {
    if (!ctx->pass1)
        ctx->total += size;
}

static int visited_mark(const MemCtx *ctx) {
    return ctx->pass1 ? MARK_EXTERNAL : MARK_DEEP;
}

/* 大小公式直接复用 Lua 内部宏（lstring.h/lfunc.h），与分配时的口径一致，
   避免和内部实现漂移；Table/Proto/thread 无现成宏，参照 lgc.c 手写 */
static size_t get_string_size(const TString *s) {
    return sizelstring(tsslen(s));
}

static size_t get_table_size(const Table *t) {
    size_t size = sizeof(Table)
                  + sizeof(TValue) * t->sizearray
                  + sizeof(Node) * cast(size_t, allocsizenode(t));
    return size;
}

static size_t get_userdata_size(const Udata *ud) {
    return sizeludata(ud->len);
}

static size_t get_thread_size(const lua_State *th) {
    size_t size = LUA_EXTRASPACE
                  + sizeof(lua_State)
                  + sizeof(TValue) * th->stacksize
                  + sizeof(CallInfo) * th->nci;
    return size;
}

static size_t get_lua_closure_size(const LClosure *cl) {
    return (size_t) sizeLclosure(cl->nupvalues);
}

static size_t get_c_closure_size(const CClosure *cl) {
    return (size_t) sizeCclosure(cl->nupvalues);
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

/* 直接按对象指针计入一个字符串（用于 Proto 内部持有的 TString） */
static void count_string_obj(MemCtx *ctx, const TString *s) {
    if (!s || ctx_mark_get(ctx, s) != MARK_NONE)
        return;
    ctx_mark_set(ctx, s, visited_mark(ctx));
    add_size(ctx, get_string_size(s));
}

static void count_proto(MemCtx *ctx, const Proto *p) {
    int i;
    if (!p || ctx_mark_get(ctx, p) != MARK_NONE)
        return;
    ctx_mark_set(ctx, p, visited_mark(ctx));
    add_size(ctx, get_proto_size(p));
    count_string_obj(ctx, p->source);
    for (i = 0; i < p->sizek; ++i) {
        const TValue *k = &p->k[i];
        if (ttisstring(k))
            count_string_obj(ctx, tsvalue(k));
    }
    for (i = 0; i < p->sizelocvars; ++i)
        count_string_obj(ctx, p->locvars[i].varname);
    for (i = 0; i < p->sizeupvalues; ++i)
        count_string_obj(ctx, p->upvalues[i].name);
    for (i = 0; i < p->sizep; ++i)
        count_proto(ctx, p->p[i]);
}

static void queue_push(MemCtx *ctx, int idx) {
    lua_pushvalue(ctx->L, idx);
    lua_rawseti(ctx->L, ctx->queue_idx, ++ctx->queue_n);
}

static void visit_value(MemCtx *ctx, int idx) {
    lua_State *L = ctx->L;
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
        case LUA_TSTRING: {
            const char *ptr = lua_tostring(L, idx);
            const TString *s;
            if (!ptr) return;
            s = (const TString *) (ptr - sizeof(UTString));
            if ((const void *) s == ctx->target_ptr) return;
            if (ctx_mark_get(ctx, s) != MARK_NONE) return;
            ctx_mark_set(ctx, s, visited_mark(ctx));
            add_size(ctx, get_string_size(s));
            return;
        }
        case LUA_TTHREAD: {
            lua_State *co = lua_tothread(L, idx);
            if (!co || (const void *) co == ctx->target_ptr) return;
            if (ctx_mark_get(ctx, co) != MARK_NONE) return;
            ctx_mark_set(ctx, co, visited_mark(ctx));
            add_size(ctx, get_thread_size(co));
            queue_push(ctx, idx);
            return;
        }
        case LUA_TTABLE: {
            const Table *t = (const Table *) lua_topointer(L, idx);
            int level;
            if (!t || t == ctx->queue_ptr || t == ctx->eph_ptr
                || (const void *) t == ctx->target_ptr)
                return;
            level = ctx_mark_get(ctx, t);
            if (level == MARK_DEEP || level == MARK_EXTERNAL) return;
            if (level == MARK_NONE) /* SHALLOW 表示自身已作为元表计过，只补内容 */
                add_size(ctx, get_table_size(t));
            ctx_mark_set(ctx, t, visited_mark(ctx));
            queue_push(ctx, idx);
            return;
        }
        case LUA_TUSERDATA: {
            const void *ptr = lua_touserdata(L, idx);
            const Udata *ud;
            if (!ptr) return;
            ud = (const Udata *) ((const char *) ptr - sizeof(UUdata));
            if ((const void *) ud == ctx->target_ptr) return;
            if (ctx_mark_get(ctx, ud) != MARK_NONE) return;
            ctx_mark_set(ctx, ud, visited_mark(ctx));
            add_size(ctx, get_userdata_size(ud));
            queue_push(ctx, idx);
            return;
        }
        case LUA_TFUNCTION: {
            const Closure *cl;
            if (lua_iscfunction(L, idx)) {
                const char *upname = lua_getupvalue(L, idx, 1);
                if (upname == NULL) /* 用 Lua C API 判断轻量 C 函数，无独立 GC 对象 */
                    return;
                lua_pop(L, 1);
            }
            cl = (const Closure *) lua_topointer(L, idx);
            if (!cl || (const void *) cl == ctx->target_ptr) return;
            if (ctx_mark_get(ctx, cl) != MARK_NONE) return;
            ctx_mark_set(ctx, cl, visited_mark(ctx));
            if (cl->c.tt == LUA_TLCL)
                add_size(ctx, get_lua_closure_size(&cl->l));
            else
                add_size(ctx, get_c_closure_size(&cl->c));
            queue_push(ctx, idx);
            return;
        }
        default:
            return;
    }
}

static void handle_metatable(MemCtx *ctx, int obj_idx, int *weak_key, int *weak_val) {
    lua_State *L = ctx->L;
    if (weak_key) *weak_key = 0;
    if (weak_val) *weak_val = 0;
    if (!lua_getmetatable(L, obj_idx))
        return;
    if (ctx->deep_mt) {
        visit_value(ctx, -1);
    } else {
        const Table *mt = (const Table *) lua_topointer(L, -1);
        if (mt && mt != ctx->queue_ptr && mt != ctx->eph_ptr
            && ctx_mark_get(ctx, mt) == MARK_NONE) {
            ctx_mark_set(ctx, mt, MARK_SHALLOW);
            add_size(ctx, get_table_size(mt));
        }
    }
    if (weak_key || weak_val) {
        lua_pushstring(L, "__mode");
        lua_rawget(L, -2);
        if (lua_type(L, -1) == LUA_TSTRING) {
            const char *mode = lua_tostring(L, -1);
            if (weak_key && strchr(mode, 'k')) *weak_key = 1;
            if (weak_val && strchr(mode, 'v')) *weak_val = 1;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

/* 把一个内部 TValue 过一遍 visit_value（经栈中转以复用其逻辑）。
   非可回收类型（数字/布尔/nil/轻量 C 函数值）直接跳过，不进栈 */
static void visit_tvalue(MemCtx *ctx, const TValue *v) {
    lua_State *L = ctx->L;
    if (!iscollectable(v))
        return;
    setobj2s(L, L->top, v);
    api_incr_top(L);
    visit_value(ctx, -1);
    lua_pop(L, 1);
}

static void expand_table(MemCtx *ctx, int idx) {
    lua_State *L = ctx->L;
    const Table *t = (const Table *) lua_topointer(L, idx);
    int weak_key, weak_val;
    unsigned int i;
    unsigned int nsize;
    int deferred = 0;
    handle_metatable(ctx, idx, &weak_key, &weak_val);
    if (!t || (weak_key && weak_val)) /* 全弱表：内容不被该表强持有 */
        return;
    if (!weak_val) {
        /* 数组部分的 key 是整数，不可回收，条目不受 __mode="k" 影响 */
        for (i = 0; i < t->sizearray; ++i)
            visit_tvalue(ctx, &t->array[i]);
    }
    nsize = (unsigned int) allocsizenode(t);
    for (i = 0; i < nsize; ++i) {
        const Node *n = gnode(t, i);
        const TValue *k = gkey(n);
        if (ttisnil(gval(n)))
            continue;
        if (!weak_key) {
            visit_tvalue(ctx, k);
            if (!weak_val)
                visit_tvalue(ctx, gval(n));
        } else if (!iscollectable(k) || ttisstring(k)) {
            /* 与 lgc.c iscleared 一致：非 GC key 和字符串 key 不构成弱引用 */
            visit_tvalue(ctx, k);
            visit_tvalue(ctx, gval(n));
        } else if (ctx_mark_get(ctx, gcvalue(k)) != MARK_NONE) {
            visit_tvalue(ctx, gval(n)); /* key 已可达，ephemeron 条目成立 */
        } else {
            deferred = 1; /* key 暂未见可达，留给 converge_ephemerons 复查 */
        }
    }
    if (deferred) {
        lua_pushvalue(L, idx);
        lua_rawseti(L, ctx->eph_idx, ++ctx->eph_n);
    }
}

static void expand_userdata(MemCtx *ctx, int idx) {
    lua_State *L = ctx->L;
    handle_metatable(ctx, idx, NULL, NULL);
    lua_getuservalue(L, idx); /* 返回 nil 也会压栈，统一弹掉 */
    visit_value(ctx, -1);
    lua_pop(L, 1);
}

static void expand_function(MemCtx *ctx, int idx) {
    lua_State *L = ctx->L;
    const Closure *cl = (const Closure *) lua_topointer(L, idx);
    int i, n;
    if (!cl) return;
    if (cl->c.tt == LUA_TLCL) {
        const LClosure *lcl = &cl->l;
        count_proto(ctx, lcl->p);
        /* UpVal 是独立分配的对象，可被多个闭包共享，按指针去重计入 */
        for (i = 0; i < lcl->nupvalues; ++i) {
            const UpVal *uv = lcl->upvals[i];
            if (uv && ctx_mark_get(ctx, uv) == MARK_NONE) {
                ctx_mark_set(ctx, uv, visited_mark(ctx));
                add_size(ctx, sizeof(UpVal));
            }
        }
        n = lcl->nupvalues;
    } else {
        n = cl->c.nupvalues;
    }
    /* upvalue 引用的值同样计入（Lua/C 闭包通用） */
    for (i = 1; i <= n; ++i) {
        if (lua_getupvalue(L, idx, i) == NULL)
            break;
        visit_value(ctx, -1);
        lua_pop(L, 1);
    }
}

/* 参照 lgc.c traversethread：协程栈上 stack..top 的存活值均被其强持有 */
static void expand_thread(MemCtx *ctx, int idx) {
    lua_State *L = ctx->L;
    lua_State *co = lua_tothread(L, idx);
    ptrdiff_t i, n;
    if (!co || co->stack == NULL)
        return;
    n = co->top - co->stack;
    for (i = 0; i < n; ++i) {
        /* 每轮重新取 co->stack：co == L 时向 L 压栈可能触发栈重分配 */
        setobj2s(L, L->top, co->stack + i);
        api_incr_top(L);
        visit_value(ctx, -1);
        lua_pop(L, 1);
    }
}

/* 出队展开直到队列为空（表/userdata/闭包/协程的子引用在这里铺开） */
static void drain_queue(MemCtx *ctx) {
    lua_State *L = ctx->L;
    while (ctx->queue_n > 0) {
        int obj_idx;
        lua_rawgeti(L, ctx->queue_idx, ctx->queue_n);
        lua_pushnil(L);
        lua_rawseti(L, ctx->queue_idx, ctx->queue_n); /* 清槽位，队列表保持精简 */
        ctx->queue_n--;
        obj_idx = lua_gettop(L);
        switch (lua_type(L, obj_idx)) {
            case LUA_TTABLE:
                expand_table(ctx, obj_idx);
                break;
            case LUA_TUSERDATA:
                expand_userdata(ctx, obj_idx);
                break;
            case LUA_TFUNCTION:
                expand_function(ctx, obj_idx);
                break;
            case LUA_TTHREAD:
                expand_thread(ctx, obj_idx);
                break;
            default:
                break;
        }
        lua_pop(L, 1);
    }
}

/* ephemeron 收敛（对应 lgc.c convergeephemerons）：weak-key 表的 value
   只有在 key 可达时才被保活。展开时 key 未标记的表挂起在 eph 列表里，
   这里反复复查——新补入的 value 可能让更多 key 变为可达——直到不动点 */
static void converge_ephemerons(MemCtx *ctx) {
    lua_State *L = ctx->L;
    int changed = 1;
    while (changed) {
        lua_Integer idx;
        changed = 0;
        for (idx = 1; idx <= ctx->eph_n; ++idx) {
            const Table *t;
            unsigned int i, nsize;
            lua_rawgeti(L, ctx->eph_idx, idx);
            t = (const Table *) lua_topointer(L, -1);
            lua_pop(L, 1); /* 表由 eph 列表锚定，弹栈后指针仍有效 */
            if (!t)
                continue;
            nsize = (unsigned int) allocsizenode(t);
            for (i = 0; i < nsize; ++i) {
                const Node *n = gnode(t, i);
                const TValue *k = gkey(n);
                const TValue *v = gval(n);
                int vmark;
                if (ttisnil(v) || !iscollectable(v))
                    continue;
                if (!iscollectable(k) || ttisstring(k))
                    continue; /* 强条目已在 expand_table 处理过 */
                if (ctx_mark_get(ctx, gcvalue(k)) == MARK_NONE)
                    continue; /* key 仍不可达，条目不成立 */
                vmark = ctx_mark_get(ctx, gcvalue(v));
                if (vmark == MARK_DEEP || vmark == MARK_EXTERNAL)
                    continue; /* value 在当前 pass 已处理 */
                visit_tvalue(ctx, v);
                changed = 1;
            }
        }
        if (changed)
            drain_queue(ctx); /* 展开新 value 可能标记新 key，再扫一轮 */
    }
}

/* 取目标对象的 GC 对象指针，用作 pass1 的不可穿透边界。
   必须与 visit_value 里各类型的去重指针同口径：
   字符串的 lua_topointer 返回 NULL，从字符内容指针反推 TString；
   userdata 的 lua_topointer 返回数据区地址，需回退到 Udata 头 */
static const void *target_gcobj_ptr(lua_State *L, int idx) {
    switch (lua_type(L, idx)) {
        case LUA_TSTRING: {
            const char *ptr = lua_tostring(L, idx);
            return ptr ? (const void *) (ptr - sizeof(UTString)) : NULL;
        }
        case LUA_TUSERDATA: {
            const char *ptr = (const char *) lua_touserdata(L, idx);
            return ptr ? (const void *) (ptr - sizeof(UUdata)) : NULL;
        }
        default:
            return lua_topointer(L, idx);
    }
}

static size_t traverse(lua_State *L, int target_idx, int deep_mt, int retained) {
    MemCtx ctx;
    ctx.L = L;
    ctx.queue_n = 0;
    ctx.total = 0;
    /* retained：元表一律深遍历，pass2 靠 EXTERNAL 标记排除共享元表 */
    ctx.deep_mt = deep_mt || retained;
    ctx.pass1 = 0;
    ctx.target_ptr = NULL;
    ctx.set = ptr_set_push_new(L);
    lua_newtable(L); /* 待展开队列（两个 pass 共用，pass1 结束时必为空） */
    ctx.queue_idx = lua_gettop(L);
    ctx.queue_ptr = lua_topointer(L, ctx.queue_idx);
    lua_newtable(L); /* 挂起的 weak-key 表列表。跨 pass 保留：外部弱键表中
                        key 仅经目标可达的条目，其 value 随目标死亡，
                        要靠 pass2 的收敛补入 retained */
    ctx.eph_idx = lua_gettop(L);
    ctx.eph_ptr = lua_topointer(L, ctx.eph_idx);
    ctx.eph_n = 0;

    if (retained) {
        ctx.pass1 = 1;
        ctx.target_ptr = target_gcobj_ptr(L, target_idx);
        lua_pushvalue(L, LUA_REGISTRYINDEX);
        visit_value(&ctx, -1);
        lua_pop(L, 1);
        lua_pushthread(L);
        visit_value(&ctx, -1);
        lua_pop(L, 1);
        drain_queue(&ctx);
        converge_ephemerons(&ctx);
        /* pass2：从目标出发正常计数，EXTERNAL 对象在 visit_value 跳过 */
        ctx.pass1 = 0;
        ctx.target_ptr = NULL;
    }

    visit_value(&ctx, target_idx);
    drain_queue(&ctx);
    converge_ephemerons(&ctx);
    ptr_set_dispose(L, ctx.set); /* 立即释放大块内存，userdata 交给 GC */
    lua_pop(L, 3); /* eph 列表 + 队列表 + PtrSet userdata */
    return ctx.total;
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

static int protected_traverse(lua_State *L) {
    size_t size;
    /* 工作队列下单次迭代栈占用是常数，但仍显式预留，保证任何调用环境下安全 */
    luaL_checkstack(L, 16, "lua_memory_tool");
    size = traverse(L, 1, lua_toboolean(L, 2), lua_toboolean(L, 3));
    lua_pushinteger(L, (lua_Integer) size);
    return 1;
}

/* 组装保护调用并在 GC 暂停下执行遍历，GetReachableSize/GetRetainedSize 共用 */
static int run_traverse_protected(lua_State *L, int deep_mt, int retained) {
    luaL_checkany(L, 1);
    lua_pushcfunction(L, protected_traverse);
    lua_pushvalue(L, 1);
    lua_pushboolean(L, deep_mt);
    lua_pushboolean(L, retained);
    /* 统计期间暂停 GC：遍历中的分配可能触发 GC 清理弱表条目，
       破坏正在进行的表遍历，也会让统计结果不稳定 */
    const int gc_running = lua_gc(L, LUA_GCISRUNNING, 0);
    lua_gc(L, LUA_GCSTOP, 0);
    const int status = lua_pcall(L, 3, 1, 0);
    if (gc_running)
        lua_gc(L, LUA_GCRESTART, 0);
    if (status != LUA_OK)
        return lua_error(L); /* 错误对象已在栈顶，恢复 GC 后原样重抛 */
    return 1; /* 栈顶即结果整数 */
}

/* GetReachableSize(obj [, deep_metatable]) -> integer */
static int l_get_reachable_size(lua_State *L) {
    /* 下标 2 必须在组装压栈前读取 */
    return run_traverse_protected(L, lua_toboolean(L, 2), 0);
}

/* GetRetainedSize(obj) -> integer */
static int l_get_retained_size(lua_State *L) {
    return run_traverse_protected(L, 0, 1);
}

/* GetShallowSize(obj) -> integer，仅自身结构大小 */
static int l_get_shallow_size(lua_State *L) {
    const size_t size = calc_shallow_size(L, 1);
    lua_pushinteger(L, (lua_Integer) size);
    return 1;
}

#else /* LUA_VERSION_NUM != 503：本工具依赖 5.3 内部布局，其他版本给出明确报错 */

static int l_unsupported(lua_State *L) {
    return luaL_error(L, "lua_memory_tool only supports Lua 5.3 (current: %s)", LUA_VERSION);
}

static int l_get_reachable_size(lua_State *L) {
    return l_unsupported(L);
}

static int l_get_retained_size(lua_State *L) {
    return l_unsupported(L);
}

static int l_get_shallow_size(lua_State *L) {
    return l_unsupported(L);
}

#endif /* LUA_VERSION_NUM == 503 */

static const luaL_Reg lua_mem_util_funcs[] = {
    {"GetShallowSize", l_get_shallow_size},     /* 自身结构大小（浅表），不含引用内容 */
    {"GetReachableSize", l_get_reachable_size}, /* 强可达引用图总占用 */
    {"GetRetainedSize", l_get_retained_size},   /* 独占大小（retained set） */
    {NULL, NULL}
};

LUALIB_API int luaopen_lua_memory_tool(lua_State *L) {
    const luaL_Reg *reg;
    lua_createtable(L, 0, 3);
    for (reg = lua_mem_util_funcs; reg->name != NULL; ++reg) {
        lua_pushcfunction(L, reg->func);
        lua_setfield(L, -2, reg->name);
    }
    return 1;
}
