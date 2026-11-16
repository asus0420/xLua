# XLuaFastCall

生产实现位于本仓库；TestLua 是验证工程，两套原生库不能互换。原生代码与 XLua 使用同一个 Lua 5.3.5 核心，要求小端、默认 Lua 数值配置，不支持 LuaJIT。

## 代码结构

- Runtime/Binding.cs：每个 LuaEnv 的生命周期、注册、ResetReceiver。
- Runtime/Binding.Configured.cs：原 XLua 方法表接入和返回支持。
- Runtime/ProxyRegistry.cs：对象句柄、代数和类型检查。
- Runtime/XLuaFastCallAttribute.cs：实例方法上的 [XLua.XLuaFastCall] 标记。
- Runtime/Native/NativeAbi.cs：共享结构和 ABI 校验。
- Runtime/Native/CallFrame.cs：参数、结果和异常处理。
- Runtime/Native/LuaStringCodec.cs：短字符串缓存、UTF-8、返回缓冲。
- Editor/XLuaFastCallGenerator.cs：XLua Generate Code 扩展。
- build/luaextension/lua_fast_call_bridge.c：原生实现。

指针操作集中在 XLuaFastCall.Native 程序集，仅此 asmdef 开启 unsafe；Binding 和生成回调不使用指针。Native 不依赖 XLua 或 UnityEngine，通过 InternalsVisibleTo 向 Assembly-CSharp 开放内部类型。自定义业务程序集名需要配套修改引用和友元配置。

## 配置与生成

在 public 实例方法上加 [XLua.XLuaFastCall]，类型仍加入 XLua LuaCallCSharp 配置，再执行 XLua/Generate Code。旧的 Editor 字典配置已迁移为方法标记，不同时维护两套选择规则。

仅支持 int、byte、long、float、double、bool、string；Vector 和枚举不能出现在参数、ref/out 或返回值签名中，必须使用显式数字接口；支持 ref/out，最多 32 输入、16 输出（返回值加全部 ref/out）。上限由 Binding.MaxInputs/MaxOutputs 与生成器共用。同名重载只标记一个具体签名。params、in、静态和泛型方法不支持。标记不支持的签名时生成直接报错。未标记的方法继续走原 Wrap。

支持有默认值的可选输入参数，且必须构成输入序列的连续后缀；只有 [Optional] 而没有默认值、可选 ref/out、可选之后又出现必选输入的签名均拒绝。生成回调通过 CallFrame.ArgCount 按实参数量分支，直接省略未提供的实参，由 C# 编译器补默认值，不把默认值渲染为字面量。

带可选输入的方法启用原生严格检查：参数个数须在必选数与总输入数之间，数字仅接受 Lua number，bool 仅接受 boolean，string 接受 string/nil。显式 nil 计入个数，不代表省略；字符串 nil 传 null。参数错误先于 receiver 查找报出，文本与原 Wrap 一致：`invalid arguments to <CsFullTypeName>.<MethodName>!`，不附带文件/行号。不带默认参数的方法保留原来的转换和 Wrong argument count 错误。

业务项目负责创建 XLuaFastCall Lua 表及 _RegisterType 安装接口、启动开关和对象生命周期。本目录不带业务安装脚本、示例回调、测试计数器、Benchmark 或 Diagnostics。

输出在 GeneratorConfig.common_path/XLuaFastCall/。保留旧 Register(name, callback, kinds, handleField) 重载，已有无默认值回调可继续编译。需要默认参数支持的业务项目再按自身配置生成回调；本次源码同步未执行生成，也未修改本仓库 XLua/Gen。

## ABI 与发布

生产 ABI 为 **1007**：Slot 16 字节，Cell 24 字节；Frame 按 Receiver、Cells、Error、Count、Reserved 排列，Count 为包含 self 的 Lua 实参数量，Reserved 显式补齐以统一 32 位 GCC/MSVC 对齐差异；64 位 Frame 为 32 字节，32 位为 24 字节。Cell 的 Kind/Size 偏移为 8/16。

sd_layout(int*) **始终写 12 个 int，保留历史索引**，第 5 项恢复为 Frame.Count 偏移，第 9 项 Token 保留为零。旧库先写布局才返回版本，不能改变查询数组容量。版本、大小和字段偏移共同校验。

生产入口为 sd_open(L)、sd_install(L,name,callback,kinds,count,required,handleField,strictName)。strictName 为 NULL 时要求 required=count，保持非严格模式；非 NULL 时为完整类型名加方法名，启用严格模式。注册仍只接受生成器支持的标量类型，并校验 required 范围。没有 Token、allowExtra、对象/Table 参数分支、验证计数器或测试注册入口。注册直接使用生成的静态委托数组元素，保证取函数指针与保活的是同一委托实例（含 Mono）。

TestLua ABI 为 **1006**，保留测试所需 Token、扩展类型和验证计数；其 Frame 布局及入口与生产版不同，不能直接复制 TestLua 二进制。两套库必须与各自 C# 配套。更换库后重启加载它的编辑器/进程。先 Dispose Binding，再 Dispose LuaEnv。

2026-11-16 本次仅同步生产源码和本文档，未编译或更新任何原生库，也未生成 XLua/Gen。旧 ABI 1004 原生库会被新托管端拒绝，使用前由项目维护者重建匹配的生产库。

## 安全边界

- 每层独立 4 KB 缓冲；返回字符串和错误文本复制完成前不释放 depth，防止 GC 重入覆盖。
- 16 输出加 4 个临时槽已达到 LUA_MINSTACK=20 边界，再次扩容必须显式检查 Lua 栈容量。
- 字符串入栈及异常格式化使用原生受保护调用；Lua 内存错误或 __gc 错误后恢复 depth，再传播原错误。
- sd_pushlstring 在受保护范围内校验参数，非法输入同时返回 Lua 错误码和错误对象。回调只传播 ERRRUN/ERRMEM/ERRGCMM/ERRERR，拒绝 yield 和其他非法状态。
- C# 长字符串通过 sd_pushlstring 入栈。它返回错误状态，托管 finally 先释放临时内存，再由回调返回后的 C 传播错误，不从该入口 longjmp 穿过托管帧。
- CallFrame.Error 不内联；格式化失败写固定 ASCII 文本，保留 Lua 调用者文件/行号。不承诺恢复进程级致命错误。
- 参数转换可能执行 GC，因此进入业务回调前再次检查关闭状态。
- Slot 和字符串地址只在当前回调内有效；ref struct 不能替代原生内存生命周期约束。

正常标量返回不增加受保护调用。短字符串缓存、ASCII/UTF-8 快速路径和最多保留 64 KB 的 scratch 复用保留。多字符串共享当前层缓冲，超出容量才走托管长字符串路径。

本轮不改变 C# 异常的消息格式、同名重载选择规则或非严格模式 float/double 的 kind 10 严格读取。

## userdata 句柄缓存

支持整数句柄及原 XLua Proxy userdata。首次从指定属性（默认 handle）读取正整数句柄并缓存到 uservalue，后续直接读取。getter 重入后重新取得栈地址并核对 uservalue，避免覆盖其他用途。

uservalue 专用于此缓存，不要通过 Lua debug API 修改。同一 userdata 的所有 FastCall 方法使用同一对象句柄。对象池重新注册后、交给 Lua 前，调用 binding.ResetReceiver(proxy)；多个 LuaEnv 分别清理。过期句柄继续由代数和类型检查拒绝，不在热路径刷新。

## 构建与验证

CMake XLUA_FASTCALL 默认 OFF，由 Lua 5.3 平台脚本启用。沿用 make_win64_lua53.bat、make_win32_lua53.bat、make_android_lua53.sh、make_ios_lua53.sh，编入各平台 XLua 库，不生成独立优化库。

历史安全验证记录在 TestLua 的 Verification/ProductionSafety/。默认参数与 32/16 扩容的 TestLua 50 项编辑器用例及性能记录位于 Verification/DefaultArgs/RESULTS.md；它们不是本次生产二进制的运行结果。生产源码同步备份、差异和检查记录保存在 TestLua 的 Verification/DefaultArgs/ProductionSync/，不作为生产文件迁入。
