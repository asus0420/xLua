/*
 * Lua 5.3.5 的 XLuaFastCall 原生入口。
 * 注册时把 C# 回调和方法描述绑定到 Lua C 闭包。
 * 调用时读取 Lua 参数，按约定的内存布局交给 C#，再把结果或错误交回 Lua。
 * 文件末尾的 LuaTableWriter 使用独立接口，直接在 Lua 栈上创建和填写 table。
 */

#include "lauxlib.h"
#include "ldo.h"
#include "lobject.h"
#include "lstate.h"
#include "lua.h"
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// 导出符号和回调调用约定按平台声明。
#if defined(_WIN32)
#define API __declspec(dllexport)
#define SD_CALL __cdecl
#else
#define API __attribute__((visibility("default")))
#define SD_CALL
#endif

// FastCall 每次调用使用固定容量。
#define DEPTH 32   // 每个上下文的回调嵌套层数上限。
#define INPUTS 32  // 最大业务输入槽数，不含 self。
#define OUTPUTS 16 // 最大输出槽数，包含方法返回值和 ref/out 结果。

// Lua 进入 C 函数时至少提供 LUA_MINSTACK 个额外槽位。结果之外留 4 槽供临时压栈使用。
// 这是编译期预算检查。增加输出容量时，应重新核对预算并按需显式检查栈空间。
typedef char outputs_fit_min_stack[(OUTPUTS + 4 <= LUA_MINSTACK) ? 1 : -1];

// sd_layout 写入布局数组所用的索引，与 C# NativeAbi 的编号一致。
enum
{
    L_SLOT,           // Slot 的总字节数。
    L_SLOT_TAG,       // Slot.tag 的字节偏移。
    L_CELL,           // Cell 的总字节数。
    L_SLOT_LENGTH,    // Slot.length 的字节偏移。
    L_FRAME_CELLS,    // Frame.cells 的字节偏移。
    L_FRAME_COUNT,    // Frame.count 的字节偏移。
    L_FRAME_ERROR,    // Frame.error 的字节偏移。
    L_CELL_KIND,      // Cell.kind 的字节偏移。
    L_CELL_SIZE,      // Cell.size 的字节偏移。
    L_FRAME_TOKEN,    // 旧 Token 槽的保留位置，维持为零。
    L_FRAME,          // Frame 的总字节数。
    L_FRAME_RECEIVER, // Frame.receiver 的字节偏移。
    L_COUNT           // 布局数组的元素数。
};

// 生成器为输入参数指定的类型，决定原生端如何读取 Lua 值。
enum
{
    T_INTEGER = 1,       // 整数目标类型，兼容路径可用 lua_tointeger 转换。
    T_NUMBER = 2,        // 普通数值，兼容路径可用 lua_tonumber 转换。
    T_BOOL = 3,          // 布尔类型，宽松入口使用 Lua 真值规则。
    T_STRING = 7,        // 字符串类型，宽松入口可将数字转为字符串。
    T_STRICT_NUMBER = 10 // 严格数值，只接受 Lua 整数或浮点数。
};

// 托管回调填写的输出类型，决定原生端用哪个 Lua API 压栈。
enum
{
    R_INTEGER = 1, // i 保存整数。
    R_NUMBER = 2,  // n 保存浮点数。
    R_BOOL = 3,    // i 保存 0 或 1。
    R_STRING = 6   // address 和 size 保存字符串地址和字节长度。
};

// 结果槽，也用于保存数字输入的兼容转换结果。
typedef struct
{
    // 三个成员共享同一块内存。输出槽按 kind 选择读取哪个成员。
    union
    {
        int64_t i;        // 整数或以 0/1 表示的布尔值。
        double n;         // 浮点数。
        uint64_t address; // 字符串字节地址，零表示 Lua nil。
    };
    int64_t kind; // 输出类型，取值见 R_*。
    int64_t size; // 仅字符串输出使用，单位为字节。
} Cell;

// 原生端传给 C# 的本次调用信息。句柄置于首位以统一 32 位编译器的字段偏移。
typedef struct
{
    int64_t receiver; // C# 对象注册表中的句柄。
    Cell *cells;      // 前 INPUTS 槽保存兼容转换结果，后 OUTPUTS 槽保存输出。
    char *error;      // 本层缓冲区，供异常文本或字符串结果使用。
    int32_t count;    // Lua 传入的值数量，包含 self。
    int32_t reserved; // 显式补齐，统一 i386 GCC 与 MSVC/C# 的结构体大小。
} Frame;

// Lua 参数复制到这个结构后交给 C#，托管端通过 tag 解释联合体。
typedef struct
{
    union
    {
        int64_t i;        // 整数或以 0/1 表示的布尔值。
        double n;         // 浮点数。
        uint64_t address; // Lua 字符串数据地址，只在本次回调期间使用。
    };
    int32_t tag;    // 输入值标签，取值见 S_*。
    int32_t length; // 仅字符串槽使用，单位为字节。
} Slot;

// 编译期检查 C 与 C# 约定的大小和偏移。
#define SD_ASSERT(name, c) typedef char sd_assert_##name[(c) ? 1 : -1]
SD_ASSERT(slot_size, sizeof(Slot) == 16);
SD_ASSERT(slot_tag, offsetof(Slot, tag) == 8);
SD_ASSERT(slot_length, offsetof(Slot, length) == 12);
SD_ASSERT(cell_size, sizeof(Cell) == 24);
SD_ASSERT(cell_kind, offsetof(Cell, kind) == 8);
SD_ASSERT(cell_size_f, offsetof(Cell, size) == 16);
SD_ASSERT(frame_recv, offsetof(Frame, receiver) == 0);
SD_ASSERT(frame_size, sizeof(Frame) == (sizeof(void *) == 8 ? 32 : 24));

// Slot 中实际传给 C# 的值标签。它与前面的目标参数类型 T_* 分开。
enum
{
    S_NIL = 0,     // 缺失槽，或原始值未写入 Slot。数字兼容转换结果见 Cell。
    S_BOOL = 1,    // i 中的 Lua 真值，归一化为 0 或 1。
    S_NUMBER = 3,  // n 中的 Lua 浮点数。
    S_STRING = 4,  // address 和 length 中的字符串视图，空地址由托管端读为 null。
    S_INTEGER = 19 // i 中的 Lua 整数。
};

// C# 回调接收 Lua 状态、输入槽和调用信息。返回值由 invoke_method 按协议解释。
typedef int(SD_CALL *Callback)(lua_State *, const Slot *, Frame *);

// 每个 Lua 注册表持有一个 Context。不同回调深度各用一份缓冲区。
typedef struct
{
    int closed,                // 绑定已关闭时，旧闭包据此停止调用。
        depth;                 // 当前占用的回调层数，也是下一层缓冲区的索引。
    char buffers[DEPTH][4096]; // 每层 4096 字节，保存异常文本或编码后的字符串结果。
} Context;

// 注册时复制的方法描述。参数计数均不含 self。
typedef struct
{
    Callback callback; // 生成的 C# 回调。
    int count,         // 参数槽总数。
        required,      // 单方法的最少参数数，重载分支的匹配参数数。
        strict,        // 单方法是否校验传入值的 Lua 类型。
        allowExtra,    // 宽松入口是否接受多于 count 的参数。
        userdataOnly,  // 是否要求 self 为完整 userdata。
        types[INPUTS]; // 每个参数槽的目标类型，取值见 T_*。
} Method;

// Lua 注册表使用这些静态变量的地址作为键。
static char key;            // Context 实例。
static char context_mt_key, // Context 的元表。
    method_mt_key,          // Method 的元表。
    group_mt_key;           // MethodGroup 的元表。

// 创建用于识别桥接 userdata 的私有元表，并保存到注册表。
static void create_private_mt(lua_State *L, const void *mtkey)
{
    // 创建空表并压栈，作为这类 userdata 的私有元表。
    lua_newtable(L);
    // 将保护标记压栈，供 Lua 的 getmetatable 返回。
    lua_pushliteral(L, "XLuaFastCall private userdata");
    // 将栈顶字符串写入 -2 处的元表，然后弹出该字符串。
    lua_setfield(L, -2, "__metatable");
    // 以 mtkey 指针为键写入注册表，跳过 __newindex，并弹出栈顶元表。
    lua_rawsetp(L, LUA_REGISTRYINDEX, mtkey);
}

// 给当前栈顶的 userdata 设置对应的私有元表。
static void set_private_mt(lua_State *L, const void *mtkey)
{
    // 从注册表直接取出 mtkey 对应的元表并压栈。
    lua_rawgetp(L, LUA_REGISTRYINDEX, mtkey);
    // 将栈顶元表设给 -2 处的 userdata，然后弹出元表。
    lua_setmetatable(L, -2);
}

// 检查完整 userdata 的数据区大小和元表身份，匹配时返回数据区地址。
// debug 库可以访问和改写元表，因此这里还要核对数据区容量。
static void *test_private_ud(lua_State *L, int index, const void *mtkey, size_t size)
{
    int matches;
    // 固定普通负索引在当前帧内的位置。正索引和 upvalue 等伪索引保持原值。
    index = lua_absindex(L, index);
    // lua_type 读类型，lua_rawlen 读 userdata 数据区大小。lua_getmetatable 仅在元表存在时将其压栈。
    if (lua_type(L, index) != LUA_TUSERDATA || lua_rawlen(L, index) != size || !lua_getmetatable(L, index))
        return NULL;
    // 从注册表压入预期的元表。当前栈顶两项是预期元表和对象的实际元表。
    lua_rawgetp(L, LUA_REGISTRYINDEX, mtkey);
    // 直接比较 -1 和 -2 处的元表身份，跳过 __eq。
    matches = lua_rawequal(L, -1, -2);
    // 弹出两张临时压入的元表。
    lua_pop(L, 2);
    // 匹配时从原槽取得 userdata 数据区地址，栈顶保持不变。
    return matches ? lua_touserdata(L, index) : NULL;
}

// 用于闭包入口。数据不匹配时产生 Lua 错误。
static void *check_private_ud(lua_State *L, int index, const void *mtkey, size_t size)
{
    // 先核对元表与大小，不匹配时由 luaL_error 构造并抛出 Lua 错误。
    void *ud = test_private_ud(L, index, mtkey, size);
    if (!ud)
        luaL_error(L, "XLuaFastCall invalid closure userdata upvalue");
    return ud;
}

// 读取并校验注册时绑定到闭包的 Context。
static Context *check_context(lua_State *L)
{
    // lua_upvalueindex(1) 生成伪索引，用来访问当前 C 闭包绑定的 Context userdata。
    return (Context *)check_private_ud(L, lua_upvalueindex(1), &context_mt_key, sizeof(Context));
}

// 校验闭包用于查询对象句柄的属性名。
static void check_handle_field(lua_State *L)
{
    // 用 lua_type 确认第 3 个 upvalue 实际存储字符串。
    if (lua_type(L, lua_upvalueindex(3)) != LUA_TSTRING)
        luaL_error(L, "XLuaFastCall invalid handle field upvalue"); // 构造带可用调用位置的消息并抛出错误。
}

// 从 Lua 注册表查找当前上下文。查找失败返回 NULL，栈高度保持原样。
static Context *find(lua_State *L)
{
    Context *c;

    // 以静态 key 的地址查注册表，压入 Context userdata，缺失时压入 nil。
    lua_rawgetp(L, LUA_REGISTRYINDEX, &key);
    c = (Context *)test_private_ud(L, -1, &context_mt_key, sizeof(Context));
    // 弹出查询结果。注册表仍持有 Context 对应的 userdata。
    lua_pop(L, 1);
    return c;
}

// 从第一个参数 self 取出对象句柄。返回 1 表示取得句柄，0 表示无法识别 self。
static int receiver_of(lua_State *L, int64_t *out)
{
    // func 指向当前 C 闭包所在的栈槽，后一槽是 Lua 参数 1。
    const TValue *h = L->ci->func + 1;
    TValue v;

    // 旧入口支持整数句柄，Proxy 入口已要求 userdata。ttisinteger 确认类型，ivalue 直接读取整数，均不压栈。
    if (ttisinteger(h))
    {
        *out = (int64_t)ivalue(h);
        return 1;
    }
    // 用内部类型标签区分完整 userdata 和其他值，只有完整 userdata 才有关联值。
    if (!ttisfulluserdata(h))
        return 0;

    // uvalue 取得 userdata 对象，getuservalue 将其关联值复制到 C 局部变量 v，跳过 Lua 栈。
    getuservalue(L, uvalue(h), &v);
    // 关联值已经是整数时，直接取出缓存的句柄。
    if (ttisinteger(&v))
    {
        *out = (int64_t)ivalue(&v);
        return 1;
    }
    // nil 表示缓存尚未建立。有其他用途的关联值时，抛出 Lua 错误。
    if (!ttisnil(&v))
        return luaL_error(L, "XLuaFastCall Proxy uservalue is already in use");

    // lua_tostring 读取闭包第 3 个 upvalue 中的属性名。lua_getfield 读取参数 1 的该字段并压入结果，可能触发 __index。
    lua_getfield(L, 1, lua_tostring(L, lua_upvalueindex(3)));
    // 栈顶是属性值。先确认它实际是 Lua 整数，再读取数值并检查是否为正，两步均不弹栈。
    if (lua_isinteger(L, -1) && lua_tointeger(L, -1) > 0)
    {
        *out = (int64_t)lua_tointeger(L, -1);
        // 属性访问可能重入并扩容 Lua 栈。重新定位 self，再读取现有的 uservalue。
        h = L->ci->func + 1;
        getuservalue(L, uvalue(h), &v);
        // 重入后允许缓存仍为 nil，或已被写入同一个整数句柄。
        if (!ttisnil(&v) && (!ttisinteger(&v) || ivalue(&v) != *out))
        {
            // 先弹出属性查询结果，再抛出关联值冲突错误。
            lua_pop(L, 1);
            return luaL_error(L, "XLuaFastCall Proxy uservalue changed during handle lookup");
        }
        // 消耗栈顶的属性值，把它存入 self 的 uservalue。
        lua_setuservalue(L, 1);
        return 1;
    }

    // 属性值无效时只弹出临时值。
    lua_pop(L, 1);
    return 0;
}

// 对象池重新注册句柄时清除 userdata 中的缓存。成功返回 0，目标无效返回 -1。
API int sd_reset_receiver(lua_State *L, int index)
{
    int kind;
    // 查询目标槽的实际类型，完整 userdata 的类型码为 LUA_TUSERDATA。
    if (lua_type(L, index) != LUA_TUSERDATA)
        return -1;
    // 将关联值压栈，并将其 Lua 类型码返回给 kind。保留其他用途的非整数关联值。
    kind = lua_getuservalue(L, index);
    // 检查栈顶的关联值是否为实际整数，读取过程不弹栈。
    if (kind != LUA_TNIL && !lua_isinteger(L, -1))
    {
        // 弹出查询结果，目标 userdata 保持原样。
        lua_pop(L, 1);
        return -1;
    }
    // 弹出旧关联值，再压入 nil 作为替换值。
    lua_pop(L, 1);
    lua_pushnil(L);
    // 设置关联值时会弹出 nil。压入 nil 后，普通负索引需再减 1 才能定位原 userdata。
    lua_setuservalue(L, index < 0 ? index - 1 : index);
    return 0;
}

// 传给受保护字符串压栈函数的参数。
typedef struct
{
    const char *text; // 待复制的字节区地址。
    int size;         // 有效字节数。
} StringPush;

// 在原生保护范围内复制字符串并压栈。
static void push_string(lua_State *L, void *data)
{
    StringPush *s = (StringPush *)data;
    // 参数无效时由 luaL_error 抛出错误，保护调用会在 Lua 栈上留下错误对象。
    if (s->size < 0 || (!s->text && s->size))
        luaL_error(L, "XLuaFastCall invalid string buffer");
    // 按指定字节数复制字符串并压栈，允许字符串中包含零字节。
    lua_pushlstring(L, s->text, (size_t)s->size);
}

// 可从 C# 调用的字符串压栈接口。返回 Lua 状态码，失败时在栈顶留下错误对象。
API int sd_pushlstring(lua_State *L, const char *text, int size)
{
    StringPush data;
    data.text = text;
    data.size = size;

    // luaD_pcall 保护 push_string，让 Lua 错误以状态码返回给 P/Invoke。
    // savestack 将栈顶保存为相对栈底的字节偏移。末尾的 0 表示不设置错误处理函数。
    return luaD_pcall(L, push_string, &data, savestack(L, L->top), 0);
}

// 把 C# 异常文本转换成 Lua 错误字符串，有调用位置时一并附上。
static void push_exception(lua_State *L, void *data)
{
    // 压入上一层调用的文件和行号前缀，找不到位置时压入空字符串。
    luaL_where(L, 1);
    // 将带固定前缀的异常正文格式化后压栈。
    lua_pushfstring(L, "XLuaFastCall C# exception: %s", (const char *)data);
    // 将栈顶的调用位置和异常正文合成一个 Lua 字符串。
    lua_concat(L, 2);
}

// 单方法和重载分支共用的调用流程，负责参数读取、C# 回调与 Lua 返回。
static int invoke_method(lua_State *L, Context *c, Method *m)
{
    // lua_gettop 返回当前帧中的值数量。入口处这些值是原始参数，包含 self。
    int count = lua_gettop(L), i, status, d;
    int64_t receiver;
    Cell cells[INPUTS + OUTPUTS];
    Slot args[INPUTS];
    Frame frame;

    // 绑定关闭或达到嵌套上限时，luaL_error 格式化消息并抛出 Lua 错误。
    if (!c || c->closed)
        return luaL_error(L, "XLuaFastCall binding closed");
    if (c->depth >= DEPTH)
        return luaL_error(L, "XLuaFastCall nesting limit");

    // 严格入口同时检查数量和实际 Lua 类型，宽松入口在这里仅限制数量。
    if (m->strict)
    {
        int argc = count - 1;
        if (argc < m->required || argc > m->count)
            goto invalid;

        // ttis* 直接检查 TValue 的类型标签。字符串允许 nil，布尔要求实际布尔值，数值可以是整数或浮点数。
        for (i = 0; i < argc; i++)
        {
            const TValue *o = L->ci->func + i + 2;
            int t = m->types[i];
            if (t == T_STRING)
            {
                if (!ttisnil(o) && !ttisstring(o))
                    goto invalid;
            }
            else if (t == T_BOOL)
            {
                if (!ttisboolean(o))
                    goto invalid;
            }
            else if (!ttisnumber(o))
                goto invalid;
        }
    }
    // 宽松入口违反参数数量约定时，用 luaL_error 抛出带调用位置的错误。
    else if (count < m->required + 1 || (!m->allowExtra && count > m->count + 1))
        return luaL_error(L, "Wrong argument count");

    // ttisfulluserdata 直接检查 self 槽。类型或句柄校验失败时，luaL_error 会抛出对应错误。
    if (m->userdataOnly && (count < 1 || !ttisfulluserdata(L->ci->func + 1)))
        return luaL_error(L, "Expected Proxy userdata as self");
    if (!receiver_of(L, &receiver))
        return luaL_error(L, "Expected integer Proxy handle or Proxy userdata with a valid handle");
    // 首次属性访问可能重入，返回后重新检查绑定。已关闭时通过 luaL_error 抛出 Lua 错误。
    if (c->closed)
        return luaL_error(L, "XLuaFastCall binding closed during handle lookup");

    // 跳过 self，把已传入的业务参数填入 Slot。宽松入口忽略超出描述的参数。
    for (i = 0; i < m->count && i + 1 < count; i++)
    {
        int index = i + 2, t = m->types[i];
        // 上一轮字符串转换可能触发 GC 或扩栈，每轮从当前调用帧重新定位参数。
        const TValue *o = L->ci->func + index;
        Slot *a = args + i;
        a->tag = S_NIL;
        if (t == T_STRING)
        {
            size_t length = 0;
            const char *text;
            // 实际为字符串时，直接从内部 TString 读取字节地址和长度，栈顶保持不变。
            if (ttisstring(o))
            {
                TString *s = tsvalue(o);
                text = getstr(s);
                length = tsslen(s);
            }
            else
            {
                // lua_tolstring 不增减栈槽。数字会在原槽转成字符串，转换失败时返回 NULL。
                // 成功时返回字节地址和长度，转换后的读取使用返回地址。
                text = lua_tolstring(L, index, &length);
            }
            // 长度超出 Slot.length 能表示的范围时，通过 luaL_error 抛出错误。
            if (length > INT_MAX)
                return luaL_error(L, "String argument too large");
            a->address = (uint64_t)(uintptr_t)text;
            a->length = (int32_t)length;
            a->tag = S_STRING;
        }
        else if (t == T_BOOL)
        {
            // l_isfalse 只把 nil 和 false 判为假。取反后写入 0 或 1。
            a->i = !l_isfalse(o);
            a->tag = S_BOOL;
        }
        // ttisinteger 和 ttisfloat 区分两种数值标签，ivalue 和 fltvalue 直接取值，均不改变栈顶。
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
            // lua_tointeger 和 lua_tonumber 可转换数字字符串，失败时返回 0，均不弹栈。
            // 兼容结果写入 Cell，严格数值由托管回调检查 Slot 标签。
            if (t == T_INTEGER)
                cells[i].i = lua_tointeger(L, index);
            if (t == T_NUMBER)
                cells[i].n = lua_tonumber(L, index);
        }
    }

    // 将缺少的输入槽清零，使托管端无须读取不存在的 Lua 参数。
    for (; i < m->count; i++)
    {
        memset(args + i, 0, sizeof(Slot));
        memset(cells + i, 0, sizeof(Cell));
    }

    // 参数转换可能触发 __gc。若其中的回调关闭了绑定，则用 luaL_error 抛出错误。
    if (c->closed)
        return luaL_error(L, "XLuaFastCall binding closed during argument conversion");

    // 本层缓冲区一直占用到结果压栈或错误格式化完成，避免重入调用覆盖数据。
    d = c->depth++;
    frame.cells = cells;
    frame.receiver = receiver;
    frame.error = c->buffers[d];
    frame.count = count;
    frame.reserved = 0;
    status = m->callback(L, args, &frame);

    // 负数表示失败。退出前先释放本层缓冲区，再由 Lua 处理错误。
    if (status < 0)
    {
        int error;
        if (status < -1)
        {
            c->depth = d;
            // 这些编码状态已有 Lua 错误对象。只解码协议列出的错误码。
            switch (status)
            {
            case -1 - LUA_ERRRUN:
            case -1 - LUA_ERRMEM:
            case -1 - LUA_ERRGCMM:
            case -1 - LUA_ERRERR:
                // 恢复回调深度后，按解码的 Lua 状态码重新抛出已有错误。
                luaD_throw(L, -1 - status);
                break;
            default:
                // 状态码不符合回调协议时，生成新的 Lua 错误消息。
                return luaL_error(L, "XLuaFastCall invalid callback error status");
            }
        }
        // 在保护范围内格式化 C# 异常。savestack 记录失败时放置 Lua 错误对象的位置。
        error = luaD_pcall(L, push_exception, frame.error, savestack(L, L->top), 0);
        c->depth = d;
        // 恢复深度后，格式化失败时用 luaD_throw 传播原错误码。成功时由 lua_error 抛出栈顶的异常消息。
        if (error != LUA_OK)
            luaD_throw(L, error);
        return lua_error(L);
    }

    // 100 + N 表示托管端已经压入 N 个结果，只需验证栈高度并返回数量。
    if (status >= 100)
    {
        status -= 100;
        c->depth = d;
        // lua_gettop 统计参数和回调已压入的值。数量不符时由 luaL_error 抛出栈布局错误。
        if (status > OUTPUTS || lua_gettop(L) != count + status)
            return luaL_error(L, "Return stack mismatch");
        return status;
    }

    // 其余非负状态是 Cell 中的结果数量，先检查上限再逐项压栈。
    if (status > OUTPUTS)
    {
        // 结果数量超过输出槽容量时，恢复深度并通过 luaL_error 抛出错误。
        c->depth = d;
        return luaL_error(L, "Return stack mismatch");
    }
    for (i = 0; i < status; i++)
    {
        Cell *v = cells + INPUTS + i;
        switch (v->kind)
        {
        case R_INTEGER:
            // 将 Cell 中的整数作为 Lua 整数压入栈顶。
            lua_pushinteger(L, v->i);
            break;
        case R_NUMBER:
            // 将 Cell 中的浮点数作为 Lua number 压入栈顶。
            lua_pushnumber(L, v->n);
            break;
        case R_BOOL:
            // 将 Cell 中的 0 或 1 转为 Lua 布尔值并压栈。
            lua_pushboolean(L, (int)v->i);
            break;
        case R_STRING:
            // 非零地址按字节长度复制字符串，空地址映射为 nil。
            if (v->address)
            {
                // 这个封装在保护范围内复制字符串，成功时将 Lua 字符串压栈。
                int error = sd_pushlstring(L, (const char *)(uintptr_t)v->address, (int)v->size);
                if (error != LUA_OK)
                {
                    c->depth = d;
                    // 恢复回调深度后，传播字符串压栈失败的 Lua 状态码。
                    luaD_throw(L, error);
                }
            }
            else
                // 字符串地址为 0 时，压入一个 Lua nil 作为返回值。
                lua_pushnil(L);
            break;
        default:
            c->depth = d;
            // 遇到协议未定义的返回类型时，构造并抛出 Lua 错误。
            return luaL_error(L, "Unknown return kind");
        }
    }
    c->depth = d;
    return status;

invalid:
    // 复制第 4 个 upvalue 到栈顶，再由 lua_error 直接抛出，以保留 xLua Wrap 的原消息格式。
    lua_pushvalue(L, lua_upvalueindex(4));
    return lua_error(L);
}

// 单方法闭包的入口，直接使用注册时绑定的 Method。
static int invoke(lua_State *L)
{
    // upvalue 1 和 2 分别持有上下文和方法描述，先验证再读取数据区。
    Context *c = check_context(L);
    // 用第 2 个 upvalue 的伪索引找到并核对 Method userdata。
    Method *m = (Method *)check_private_ud(L, lua_upvalueindex(2), &method_mt_key, sizeof(Method));
    check_handle_field(L);
    return invoke_method(L, c, m);
}

// 重载组和分支描述连续存放在同一个 userdata 中。
typedef struct
{
    int count;         // 已注册的分支数，包含为可选参数生成的分支。
    Method methods[1]; // 占位数组，注册时按实际分支数分配尾部空间。
} MethodGroup;

// 按注册顺序匹配参数数量和类型，执行第一个匹配的重载分支。
static int invoke_group(lua_State *L)
{
    Context *c = check_context(L);
    // lua_rawlen 读取第 2 个 upvalue 的 userdata 数据区大小，再用该伪索引核对元表。
    size_t bytes = lua_rawlen(L, lua_upvalueindex(2));
    MethodGroup *group = (MethodGroup *)check_private_ud(L, lua_upvalueindex(2), &group_mt_key, bytes);
    // 当前栈上只有传入值，减去 self 得到重载匹配用的参数数。
    int argc = lua_gettop(L) - 1, branch, i;

    // 先验证数据区长度，再核对分支数和预期的总大小。校验失败时由 luaL_error 抛出错误。
    if (bytes < offsetof(MethodGroup, methods) + 2 * sizeof(Method) || group->count < 2 || group->count > 1024 ||
        bytes != offsetof(MethodGroup, methods) + (size_t)group->count * sizeof(Method))
        return luaL_error(L, "XLuaFastCall invalid method group upvalue");
    check_handle_field(L);

    // 从参数 2 开始，ttis* 直接查看 TValue 的类型标签。字符串允许 nil，数字字符串不按数字匹配。
    for (branch = 0; branch < group->count; branch++)
    {
        Method *m = &group->methods[branch];
        if (argc != m->required)
            continue;
        for (i = 0; i < argc; i++)
        {
            const TValue *v = L->ci->func + i + 2;
            int kind = m->types[i];
            if (kind == T_STRING)
            {
                if (!ttisnil(v) && !ttisstring(v))
                    break;
            }
            else if (kind == T_BOOL)
            {
                if (!ttisboolean(v))
                    break;
            }
            else if (!ttisnumber(v))
                break;
        }
        if (i == argc)
            return invoke_method(L, c, m);
    }

    // 复制第 4 个 upvalue 中预先构造的消息，再由 lua_error 抛出栈顶值。
    lua_pushvalue(L, lua_upvalueindex(4));
    return lua_error(L);
}

// 向 C# 报告 Slot、Cell 和 Frame 的内存布局，并返回 ABI 版本号。p 需容纳 L_COUNT 个整数。
API int sd_layout(int *p)
{
    // 先清零，包括必须保持为零的旧 Token 槽。
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
    p[L_FRAME] = sizeof(Frame);
    p[L_FRAME_RECEIVER] = offsetof(Frame, receiver);

    // 托管端同时核对版本号、结构体大小和字段偏移。
    return 1009;
}

// 初始化当前 Lua 注册表的 FastCall 上下文。成功返回 0，条件不满足时返回 -1。
API int sd_open(lua_State *L)
{
    Context *c;
    const uint16_t one = 1;

    // 检查小端字节序和 Lua 5.3 版本号 503。已有上下文时拒绝再次初始化。
    if (*(const uint8_t *)&one != 1)
        return -1;
    // lua_version 返回指向 Lua 版本号的指针。解引用得到 503 时表示 Lua 5.3。
    if (*lua_version(L) != 503 || find(L))
        return -1;

    // 为三种 userdata 注册各自的元表，再创建并保存上下文。
    create_private_mt(L, &context_mt_key);
    create_private_mt(L, &method_mt_key);
    create_private_mt(L, &group_mt_key);
    // 分配 Context 大小的数据区，将 userdata 压栈，并返回可写的数据区地址。
    c = (Context *)lua_newuserdata(L, sizeof(Context));
    set_private_mt(L, &context_mt_key);
    memset(c, 0, sizeof(*c));
    // 以 &key 为键直接写入注册表并弹出 userdata。注册表继续持有该对象。
    lua_rawsetp(L, LUA_REGISTRYINDEX, &key);
    return 0;
}

// 把单方法闭包安装到栈顶的目标 table。strictName 存在时启用严格参数检查。
static int install(lua_State *L, const char *name, Callback callback, const int *types, int count, int required,
                   int allowExtra, int userdataOnly, const char *handleField, const char *strictName)
{
    Context *c = find(L);
    Method *m;
    int i;

    // 只在绑定有效且没有活动回调时安装。lua_istable 确认 -1 处是目标表，栈顶保持不变。
    if (!c || c->closed || c->depth || !name || !callback || !handleField || !handleField[0] || count < 0 ||
        count > INPUTS || required < 0 || required > count || (strictName && allowExtra) ||
        (allowExtra != 0 && allowExtra != 1) || (count && !types) || !lua_istable(L, -1))
        return -1;

    // 只接受原生参数转换支持的类型。T_NUMBER 仅允许用于 Proxy 入口。
    for (i = 0; i < count; i++)
        if (types[i] != T_INTEGER && types[i] != T_BOOL && types[i] != T_STRING && types[i] != T_STRICT_NUMBER &&
            (types[i] != T_NUMBER || !userdataOnly))
            return -1;

    // 以 &key 直接从注册表把 Context 压栈，作为闭包的第 1 个 upvalue。
    lua_rawgetp(L, LUA_REGISTRYINDEX, &key);
    // 分配 Method 的数据区并把 userdata 压栈，作为第 2 个 upvalue。
    m = (Method *)lua_newuserdata(L, sizeof(Method));
    set_private_mt(L, &method_mt_key);
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

    // 将句柄属性名压栈作为第 3 个 upvalue。
    // 第 4 个用 lua_pushfstring 构造错误消息，或用 lua_pushnil 压入 nil 占位。
    lua_pushstring(L, handleField);
    if (strictName)
        lua_pushfstring(L, "invalid arguments to %s!", strictName);
    else
        lua_pushnil(L);

    // 取出栈顶四个值作为 upvalue，并将绑定 invoke 的 C 闭包压栈。
    lua_pushcclosure(L, invoke, 4);
    // 闭包在 -1，注册目标表在 -2。写入 name 字段后，弹出栈顶闭包。
    lua_setfield(L, -2, name);
    return 0;
}

// 旧版单方法注册入口，兼容整数句柄和 Proxy userdata。
API int sd_install(lua_State *L, const char *name, Callback callback, const int *types, int count, int required,
                   const char *handleField, const char *strictName)
{
    // 宽松模式沿用旧约定，要求业务参数数量与注册数量一致。
    if (!strictName && required != count)
        return -1;
    return install(L, name, callback, types, count, required, 0, 0, handleField, strictName);
}

// Proxy 专用的单方法注册入口。
API int sd_install_proxy(lua_State *L, const char *name, Callback callback, const int *types, int count, int required,
                         int allowExtra, const char *handleField, const char *strictName)
{
    // 固定要求 userdata self，是否接受多余参数由 allowExtra 决定。
    return install(L, name, callback, types, count, required, allowExtra, 1, handleField, strictName);
}

// 把同名重载注册为一个 Lua 闭包。所有分支的描述在注册时复制到原生 userdata。
API int sd_install_group(lua_State *L, const char *name, const Callback *callbacks, const int *counts,
                         const int *arities, const int *types, int branches, const char *handleField,
                         const char *strictName)
{
    Context *c = find(L);
    MethodGroup *group;
    int b, i;

    // 先核对公共参数，lua_istable 检查目标表在栈顶。再逐分支检查参数数量和类型。
    if (!c || c->closed || c->depth || !name || !callbacks || !counts || !arities || !types || !handleField ||
        !handleField[0] || !strictName || branches < 2 || branches > 1024 || !lua_istable(L, -1))
        return -1;
    for (b = 0; b < branches; b++)
    {
        if (!callbacks[b] || counts[b] < 0 || counts[b] > INPUTS || arities[b] < 0 || arities[b] > counts[b])
            return -1;
        for (i = 0; i < counts[b]; i++)
        {
            int t = types[b * INPUTS + i];
            if (t != T_INTEGER && t != T_STRICT_NUMBER && t != T_BOOL && t != T_STRING)
                return -1;
        }
    }

    // 为全部分支一次性分配空间。types 中每个分支占 INPUTS 个位置。
    // 按 &key 取出 Context 并压栈，作为第 1 个 upvalue。
    lua_rawgetp(L, LUA_REGISTRYINDEX, &key);
    // 按分支数分配一块连续数据区并压入 userdata，作为第 2 个 upvalue。
    group = (MethodGroup *)lua_newuserdata(L, offsetof(MethodGroup, methods) + branches * sizeof(Method));
    set_private_mt(L, &group_mt_key);
    group->count = branches;
    for (b = 0; b < branches; b++)
    {
        Method *m = &group->methods[b];
        memset(m, 0, sizeof(*m));
        m->callback = callbacks[b];
        m->count = counts[b];
        m->required = arities[b];
        m->userdataOnly = 1;
        for (i = 0; i < counts[b]; i++)
            m->types[i] = types[b * INPUTS + i];
    }

    // 压入第 3 个 upvalue，即查询句柄用的属性名。
    lua_pushstring(L, handleField);
    // 压入第 4 个 upvalue，即重载匹配失败时使用的消息。
    lua_pushfstring(L, "invalid arguments to %s!", strictName);
    // 消耗四个 upvalue，把绑定 invoke_group 的 C 闭包压栈。
    lua_pushcclosure(L, invoke_group, 4);
    // 将栈顶的重载闭包写入 -2 处目标表的 name 字段，然后弹出闭包。
    lua_setfield(L, -2, name);
    return 0;
}

// 关闭 FastCall 绑定。成功或已关闭返回 0，仍有活动回调时返回 -1。
API int sd_close(lua_State *L)
{
    Context *c = find(L);
    if (!c || c->closed)
        return 0;
    if (c->depth)
        return -1;

    // 保留注册表条目和 userdata。仍被 Lua 持有的闭包可读到已关闭的标记。
    c->closed = 1;
    return 0;
}

/*
 * LuaTableWriter 使用独立的协议，在 Lua 5.3 主线程的值栈上创建、写入和返回 table。
 */
#include "lstring.h"
#include "lgc.h"

// LuaTableWriter 返回给托管端的状态码。
#define TW_OK 0           // 操作成功。
#define TW_LUAERR 1       // 受保护操作失败，Lua 错误对象已压栈。
#define TW_STALE -1       // 指定槽位上的 table 与记录的对象不匹配。
#define TW_NOSTACK -2     // 无法确保 Lua 栈有足够空间。
#define TW_STATE -3       // Lua 状态不满足当前操作的要求。
#define TW_BADARGS -4     // 类型、长度或其他输入参数无效。
#define TW_NO_CALLBACK -5 // 当前不在可用的 C 调用帧中。

// 检查正索引仍指向预期的 table。
static int tw_valid(lua_State *L, int idx, const void *ptr)
{
    // lua_gettop 约束有效正索引，lua_type 确认是 table，lua_topointer 比较对象地址。三次检查均不改变栈顶。
    return L && ptr && idx > 0 && idx <= lua_gettop(L) && lua_type(L, idx) == LUA_TTABLE &&
           lua_topointer(L, idx) == ptr;
}

// 判断 L 是否为 Lua 主线程。coroutine 有独立的 lua_State 指针。
API int xlua_tw_main(lua_State *L)
{
    // G(L) 取得与协程共享的全局状态。比较其中的 mainthread 可确认 L 是否为 Lua 主线程。
    return L && L == G(L)->mainthread;
}

// 创建 table 时传给原生保护调用的容量提示。
typedef struct
{
    int array, // 数组部分的预分配容量。
        hash;  // 哈希部分的预分配容量。
} tw_new_args;

// 创建并压入一个 Lua table，分配错误由调用方的 luaD_pcall 捕获。
static void tw_do_new(lua_State *L, void *ud)
{
    tw_new_args *a = (tw_new_args *)ud;

    // 按数组和哈希部分的容量提示创建空表，并压入一个 table。
    lua_createtable(L, a->array, a->hash);
}

// 创建 table 并将其留在栈上，通过 idx 和 ptr 返回位置与对象地址。
API int xlua_tw_new(lua_State *L, int parent, const void *parent_ptr, int array, int hash, int *idx, const void **ptr)
{
    tw_new_args a;
    // 从全局状态读取 mainthread，确保操作发生在 Lua 主线程的值栈上。
    if (!L || L != G(L)->mainthread)
        return TW_STATE;
    // isLua 通过调用帧标志判断是否在执行 Lua 函数。这里要求已有 C 调用帧。
    if (L->ci == &L->base_ci || isLua(L->ci))
        return TW_NO_CALLBACK;
    if (parent && !tw_valid(L, parent, parent_ptr))
        return TW_STALE;
    if (array < 0 || hash < 0)
        return TW_BADARGS;

    // lua_checkstack 确认还有 LUA_MINSTACK + 2 个可用槽位，必要时扩容。
    // 新表留在栈上，额外空间用于满足 xLua 包装器后续压栈的需求。
    if (!lua_checkstack(L, LUA_MINSTACK + 2))
        return TW_NOSTACK;

    a.array = array;
    a.hash = hash;
    // 保护建表过程。savestack 保存原栈顶的位置，失败时在该处留下 Lua 错误对象。
    if (luaD_pcall(L, tw_do_new, &a, savestack(L, L->top), 0) != LUA_OK)
        return TW_LUAERR;
    // 新表位于栈顶。lua_gettop 返回它在当前帧中的正索引。
    *idx = lua_gettop(L);
    // lua_topointer 读取 -1 处新表的身份地址，供后续操作校验。
    *ptr = lua_topointer(L, -1);
    return TW_OK;
}

// 传给受保护写入函数的键值参数。字符串键和字符串值顺序存放在同一个字节区。
typedef struct
{
    int idx,           // 目标 table 的绝对栈索引。
        key_kind,      // 键类型，0 为整数，1 为 UTF-8 字符串。
        key_len,       // 字符串键的字节数，-1 表示 nil 键。
        value_len,     // 字符串值的字节数。
        kind,          // 值类型，见 tw_do_set 中的分支。
        child,         // 子表的绝对栈索引。
        value_slot;    // 调用方提供的栈顶值的绝对索引。
    lua_Integer key,   // 整数键。
        integer;       // 整数值，布尔值也用此字段传递。
    lua_Number number; // 浮点值。
    const char *bytes; // 键和值共用的 UTF-8 字节区。
} tw_set_args;

// 将指定字节复制为 Lua 字符串，并直接写入已预留的栈槽。
static void tw_push_string(lua_State *L, const char *bytes, int len)
{
    // 从指定字节创建或复用内部字符串对象。此调用只返回 TString 指针。
    TString *s = luaS_newlstr(L, bytes, (size_t)len);
    // 把 TString 写入 L->top 指向的空槽，再推进栈顶。GC 检查留到写表之后。
    setsvalue2s(L, L->top, s);
    L->top++;
}

// 先压入键，再按协议压入值，最后将两者写入 table。
static void tw_do_set(lua_State *L, void *ud)
{
    tw_set_args *a = (tw_set_args *)ud;
    if (a->key_kind)
    {
        // nil 键由 luaL_error 抛出 Lua 错误，避免把负长度当作字节偏移。
        if (a->key_len < 0)
            luaL_error(L, "table index is nil");
        tw_push_string(L, a->bytes, a->key_len);
    }
    else
        lua_pushinteger(L, a->key); // 将整数键压入栈顶。
    switch (a->kind)
    {
    case 0: // 将 nil 压栈作为值。写入后移除该键。
        lua_pushnil(L);
        break;
    case 1: // 将整数压栈作为值。
        lua_pushinteger(L, a->integer);
        break;
    case 2: // 将浮点数压栈作为值。
        lua_pushnumber(L, a->number);
        break;
    case 3: // 将整数按是否为零转换成布尔值并压栈。
        lua_pushboolean(L, a->integer != 0);
        break;
    case 4: // UTF-8 字符串，使用字符串键时从键数据之后开始读取。
        tw_push_string(L, a->bytes + (a->key_kind ? a->key_len : 0), a->value_len);
        break;
    case 5: // 把子表引用复制到栈顶，原槽保持不变。
        lua_pushvalue(L, a->child);
        break;
    case 6: // 复制调用方原先放入的值，保留它原来的栈槽。
        lua_pushvalue(L, a->value_slot);
        break;
    }

    // 从 -2 取键，从 -1 取值，直接写入目标表并弹出键值，跳过 __newindex。
    lua_rawset(L, a->idx);

    // 字符串已复制到 Lua。此时通过 luaC_checkGC 按需推进垃圾回收。
    if (a->key_kind || a->kind == 4)
        luaC_checkGC(L);
}

// 校验目标和键值参数，再在原生保护范围内写 table。
API int xlua_tw_set(lua_State *L, int idx, const void *ptr, int key_kind, lua_Integer key, const char *bytes,
                    int bytes_len, int key_len, int kind, lua_Integer integer, lua_Number number, int value_len,
                    lua_State *child_owner, int child, const void *child_ptr)
{
    tw_set_args a;
    if (!tw_valid(L, idx, ptr))
        return TW_STALE;
    if (key_kind < 0 || key_kind > 1 || kind < 0 || kind > 6 || key_len < -1 || value_len < 0)
        return TW_BADARGS;

    // 字符串值紧跟字符串键。读取字节前检查键长和值长是否超出缓冲区。
    if ((key_kind && key_len > 0) || kind == 4)
    {
        int prefix = key_kind && key_len > 0 ? key_len : 0;
        if (!bytes || bytes_len < prefix || value_len > bytes_len - prefix)
            return TW_BADARGS;
    }

    // 子表必须来自同一个 Lua 栈，且持有的索引仍指向该子表。
    if (kind == 5)
    {
        if (L != child_owner)
            return TW_STATE;
        if (!tw_valid(L, child, child_ptr))
            return TW_STALE;
    }

    // lua_gettop 记录借用值的正索引，后续压入键和值时它仍指向原槽。
    // 写表只消耗该值的副本，调用方负责恢复进入时的栈顶。
    a.value_slot = kind == 6 ? lua_gettop(L) : 0;
    if (kind == 6 && a.value_slot <= idx)
        return TW_BADARGS;

    // lua_checkstack 确保至少有两个额外槽位供键和值使用，失败时返回 0。
    if (!lua_checkstack(L, 2))
        return TW_NOSTACK;

    a.idx = idx;
    a.key_kind = key_kind;
    a.key = key;
    a.bytes = bytes;
    a.key_len = key_len;
    a.kind = kind;
    a.integer = integer;
    a.number = number;
    a.value_len = value_len;
    a.child = child;

    // 在保护范围内执行写表。成功时消耗临时键和值，失败时在原栈顶处留下 Lua 错误对象。
    return luaD_pcall(L, tw_do_set, &a, savestack(L, L->top), 0) == LUA_OK ? TW_OK : TW_LUAERR;
}

// 将已创建的 table 复制到栈顶，供调用方将其传递或作为 Lua 结果返回。
API int xlua_tw_push(lua_State *L, lua_State *owner, int idx, const void *ptr)
{
    // 索引属于创建时的 Lua 状态。
    if (L != owner)
        return TW_STATE;
    if (!tw_valid(L, idx, ptr))
        return TW_STALE;
    // 预留复制 table 引用所需的一个栈槽，失败时返回 0。
    if (!lua_checkstack(L, 1))
        return TW_NOSTACK;
    // 将 idx 处的 table 引用复制到栈顶，原槽仍保留。
    lua_pushvalue(L, idx);
    return TW_OK;
}
