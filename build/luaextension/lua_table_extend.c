#include "lua.h"
#include "lauxlib.h"
#include <string.h>
#include <limits.h>

#if LUA_VERSION_NUM >= 502
#define xlua_rawlen lua_rawlen
#else
#define xlua_rawlen lua_objlen
#endif


#if LUA_VERSION_NUM < 503
static int xlua_isinteger(lua_State* L, int idx)
{
	if (lua_type(L, idx) != LUA_TNUMBER) return 0;
	lua_Number n = lua_tonumber(L, idx);
	lua_Integer i = (lua_Integer)n;
	return (lua_Number)i == n;
}
#define XLUA_MAX_RAWI INT_MAX
#else
#define xlua_isinteger lua_isinteger
#define XLUA_MAX_RAWI LUA_MAXINTEGER
#endif


static int xlua_clamp_capacity(lua_State* L, int arg, lua_Integer n)
{
	if (n < 0) return 0;

	if (n > INT_MAX)
		return luaL_argerror(L, arg, "table capacity too large");

	return (int)n;
}

static int xlua_count_entries(lua_State* L, int idx)
{
	lua_Integer count = 0;
	lua_pushnil(L);
	while (lua_next(L, idx) != 0)
	{
		++count;
		lua_pop(L, 1);
	}
	if (count > INT_MAX)
		return luaL_error(L, "table too large");

	return (int)count;
}


static int xlua_table_create(lua_State* L) {
	lua_Integer narr = luaL_checkinteger(L, 1);
	lua_Integer nrec = luaL_optinteger(L, 2, 0);

	int arr = xlua_clamp_capacity(L, 1, narr);
	int rec = xlua_clamp_capacity(L, 2, nrec);

	lua_createtable(L, arr, rec);
	return 1;
}

//static int xlua_table_clear(lua_State* L) {
//	luaL_checktype(L, 1, LUA_TTABLE);
//	size_t size = xlua_rawlen(L, 1);
//	if (size > (size_t)XLUA_MAX_RAWI)
//		return luaL_error(L, "table length too large");
//	lua_Integer n = (lua_Integer)size;
//	// 数组段
//	for (lua_Integer i = n; i >= 1; i--)
//	{
//		lua_pushnil(L);
//		lua_rawseti(L, 1, i);
//	}
//	// Hash段
//	lua_pushnil(L);
//	while (lua_next(L, 1) != 0)
//	{
//		lua_pop(L, 1);			   /* 弹 value */
//		lua_pushvalue(L, -1);      /* 复制 key 用于 rawset */
//		lua_pushnil(L);
//		lua_rawset(L, 1);          /* t[key]=nil，留下原 key 供 next */
//	}
//	return 0;
//}

//static int xlua_table_empty(lua_State* L) {
//	luaL_checktype(L, 1, LUA_TTABLE);
//	lua_pushnil(L);
//	int empty = (lua_next(L, 1) == 0);
//	if (!empty) lua_pop(L, 2);  /* 弹掉 next 留下的 key/value */
//	lua_pushboolean(L, empty);
//	return 1;
//}

static int xlua_table_size(lua_State* L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_Integer count = 0;
	lua_pushnil(L);
	while (lua_next(L, 1) != 0)
	{
		++count;
		lua_pop(L, 1);
	}
	lua_pushinteger(L, count);
	return 1;
}

//static int xlua_table_keys(lua_State* L) {
//	luaL_checktype(L, 1, LUA_TTABLE);
//	lua_settop(L, 1); // 只保留第一个参数
//
//	int size = xlua_count_entries(L, 1);
//	lua_createtable(L, size, 0);            /* dst, idx 2 */
//	int i = 0;
//	lua_pushnil(L);
//	while (lua_next(L, 1) != 0) {
//		lua_pop(L, 1);          /* 弹 value */
//		lua_pushvalue(L, -1);   /* 复制 key */
//		lua_rawseti(L, 2, ++i); /* dst[++i]=key */
//	}
//	return 1;
//}

//static int xlua_table_values(lua_State* L) {
//	luaL_checktype(L, 1, LUA_TTABLE);
//	lua_settop(L, 1); // 只保留第一个参数
//
//	int size = xlua_count_entries(L, 1);
//	lua_createtable(L, size, 0);            /* dst, idx 2 */
//	int i = 0;
//	lua_pushnil(L);
//	while (lua_next(L, 1) != 0) {
//		/* stack: src dst key value */
//		lua_rawseti(L, 2, ++i); /* dst[++i]=value，自动弹 value */
//	}
//	return 1;
//}

//static int xlua_table_merge(lua_State* L) {
//	luaL_checktype(L, 1, LUA_TTABLE);   /* dst */
//	luaL_checktype(L, 2, LUA_TTABLE);   /* src */
//	lua_pushnil(L);
//	while (lua_next(L, 2) != 0) {
//		/* stack: dst src key value */
//		lua_pushvalue(L, -2);   /* 复制 key */
//		lua_pushvalue(L, -2);   /* 复制 value */
//		lua_rawset(L, 1);       /* dst[key]=value */
//		lua_pop(L, 1);          /* 弹 value，key 留给 next */
//	}
//	lua_pushvalue(L, 1);
//	return 1;
//}

static int xlua_table_indexof(lua_State* L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	luaL_checkany(L, 2);

	size_t size = xlua_rawlen(L, 1);
	if (size > (size_t)XLUA_MAX_RAWI)
		return luaL_error(L, "table length too large");

	lua_Integer n = (lua_Integer)size;
	for (lua_Integer i = 1; i <= n; i++) {
		lua_rawgeti(L, 1, i);
		if (lua_rawequal(L, -1, 2)) {  /* 跳过 __eq */
			lua_pushinteger(L, i);
			return 1;
		}
		lua_pop(L, 1);
	}
	lua_pushnil(L);
	return 1;
}

static int xlua_table_isarray(lua_State* L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_Integer count = 0;
	lua_Integer max_idx = 0;
	lua_pushnil(L);
	while (lua_next(L, 1) != 0) {
		if (!xlua_isinteger(L, -2)) {     /* 非数字 key 直接出局 */
			lua_pop(L, 2);
			lua_pushboolean(L, 0);
			return 1;
		}
		lua_Integer ik = lua_tointeger(L, -2);
		if (ik < 1) {
			lua_pop(L, 2);
			lua_pushboolean(L, 0);
			return 1;
		}

		if (ik > max_idx) max_idx = ik;
		count++;
		lua_pop(L, 1);   /* 弹 value，key 留给 next */
	}
	/* 无空洞判定：n 个键 + 最大索引正好 == n */
	lua_pushboolean(L, max_idx == count);
	return 1;
}

static int xlua_table_ismap(lua_State* L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_Integer count = 0;
	int has_any = 0;
	lua_Integer max_idx = 0;
	lua_pushnil(L);
	while (lua_next(L, 1) != 0) {
		has_any = 1;
		if (!xlua_isinteger(L, -2)) {     /* 非数字 key → 立刻判 map */
			lua_pop(L, 2);
			lua_pushboolean(L, 1);
			return 1;
		}
		lua_Integer ik = lua_tointeger(L, -2);
		if (ik < 1) {
			lua_pop(L, 2);
			lua_pushboolean(L, 1);
			return 1;
		}
		if (ik > max_idx) max_idx = ik;
		count++;
		lua_pop(L, 1);
	}
	if (!has_any) {
		lua_pushboolean(L, 0);                    /* 空表 → 非 map */
	}
	else {
		lua_pushboolean(L, max_idx != count);  /* 有空洞也算 map */
	}
	return 1;
}

static int xlua_table_contains(lua_State* L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	luaL_checkany(L, 2);

	lua_pushnil(L);
	while (lua_next(L, 1) != 0) {
		/* stack: t, key, value */
		if (lua_rawequal(L, -1, 2)) {     /* 跳过 __eq，原值比较 */
			lua_pop(L, 2);                /* 清掉 key 和 value */
			lua_pushboolean(L, 1);
			return 1;
		}
		lua_pop(L, 1);                    /* 弹 value，key 留给 next */
	}
	lua_pushboolean(L, 0);
	return 1;
}

static const luaL_Reg xlua_table_funcs[] = {
	{"create", xlua_table_create},		// 创建固定长度的数组
	//{"clear", xlua_table_clear},		// 清理table，不用通过创建新表的方式 tab = {}
	//{"isempty", xlua_table_empty},		// 判断table 是否为空（需要验证一下二进制模式下配置表是否能正确表现） 
	{"size", xlua_table_size},			// 获取table 长度，支持hash段
	//{"keys", xlua_table_keys},			// 将原table的key存入新创建的table中， 类似c#字典的用法
	//{"values", xlua_table_values},		// 将原table的value存入新创建的table中， 类似c#字典的用法
	//{"merge", xlua_table_merge},		// 合并两张表， (dst, src) 将src的内容写入到dst中，同名时src会覆盖dst
	{"indexof", xlua_table_indexof},	// 数组段查值
	{"isarray", xlua_table_isarray},	// table是否为数组，空表为数组
	{"ismap", xlua_table_ismap},		// table是否为字典，空表为数组
	{"contains", xlua_table_contains},	// table是否存在某个value

	{NULL, NULL}
};

static void inject_into_global(lua_State* L, const char* libname, const luaL_Reg* funcs) {
	lua_getglobal(L, libname);
	if (lua_istable(L, -1)) {
		for (const luaL_Reg* l = funcs; l->name != NULL; l++)
		{
			lua_pushcfunction(L, l->func);
			lua_setfield(L, -2, l->name);
		}
	}
	lua_pop(L, 1);
}

/* 把 xlua_table_funcs中的函数 注入到全局 table 库，必须在 luaL_openlibs 之后调用 */
void xlua_inject_table(lua_State* L) {
	inject_into_global(L, "table", xlua_table_funcs);
}
