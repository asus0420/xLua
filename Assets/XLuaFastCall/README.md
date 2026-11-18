# XLuaFastCall

正式实现，与 xLua 使用同一个 Lua 5.3.5 核心。要求小端、默认 Lua 数值配置，不支持 LuaJIT。

## 代码结构

- Runtime/Binding.cs：LuaEnv 生命周期、单方法/方法组注册、ResetReceiver。
- Runtime/Binding.Configured.cs：接入原 xLua 方法表，处理结果输出。
- Runtime/ProxyRegistry.cs：对象句柄、代数和接收者类型检查。
- Runtime/XLuaFastCallAttribute.cs：实例方法的 [XLua.XLuaFastCall] 标记。
- Runtime/Native/NativeAbi.cs：生产共享结构和 ABI 校验。
- Runtime/Native/CallFrame.cs：参数读取、结果和异常处理。
- Runtime/Native/LuaStringCodec.cs：短字符串缓存、平台解码、返回缓冲。
- Editor/XLuaFastCallGenerator.cs：通过现有 GenCodeMenu 接入生成流程。
- Editor/XLuaMethodGroups.cs：只在编辑器读取 xLua 实际方法分组及分支顺序。
- Editor/XLuaFastCallAutoTags.cs：按精确签名为项目配置的源码生成标签。
- build/luaextension/lua_fast_call_bridge.c：正式原生实现，编入 xlua 库。

指针操作集中在 XLuaFastCall.Native 程序集，仅此 asmdef 开启 unsafe；Binding 和生成回调不使用指针。Native 不依赖 XLua 或 UnityEngine，通过 InternalsVisibleTo 向 Assembly-CSharp 开放内部类型。自定义业务程序集须配套设置引用和友元配置。

## 生成与整组选择

类型加入 xLua LuaCallCSharp 配置，在需要优化的 public 实例方法上添加 [XLua.XLuaFastCall]，执行 XLua/Generate Code。仅支持 int、byte、long、float、double、bool、string，以及 ref/out；最多 32 个输入槽、16 个输出槽。Vector、枚举、LuaTable、params、in、静态及泛型方法不属于 FastCall 支持范围。

按 xLua 实际导出的方法组选择优化范围：

- 同组全部重载均被选择且签名均支持，才整体生成 FastCall。多个受支持的同名重载可以全部标记。
- 任一重载未选择或不支持，整组保留原 Wrap；不生成部分 FastCall 再回退另一部分 Wrap 的混合入口。
- generation-decisions.txt 记录每组 FAST / SKIP 及跳过原因。BlackList、配置过滤、静态/实例分组及扩展方法的分组结果来自 xLua。
- XLuaMethodGroups 在生成阶段只读反射调用 Generator.getClassInfo，读取 Overloads / DefaultValues。游戏业务调用不使用反射。升级 xLua 时需要确认该私有生成接口；接口或数据结构不兼容时生成明确报错。
- 输出在 GeneratorConfig.common_path/XLuaFastCall/。普通 xLua Wrap 的源码和模板不需要修改。

## 参数与重载行为

普通单分支没有额外的重载类型检查：允许少传和多传；缺失数值为 0，bool 为 false，string 为 null；多余参数忽略。浮点宽松转换使用 CallFrame.Number，沿用 Lua 数字转换规则。

重载组和可选参数展开的多分支入口，先按 xLua 顺序检查原始 Lua 类型和数量，再转换参数并调用唯一分支。多个数字重载同时匹配时选择原 Wrap 排在前面的分支，不自行进行最佳类型推断。转换失败或业务异常不会尝试下一分支。

可选输入必须有默认值并构成连续后缀。生成回调省略未提供实参，由 C# 编译器补默认值。显式 nil 计入实参数量，不等于省略；string 的 nil 是 null。out 不占 Lua 输入，ref 占输入；先返回函数结果，再按声明顺序返回 out/ref。

生成的 Proxy 入口只接受 userdata self，支持 p:Method(x) 或 p.Method(p,x)，拒绝整数句柄作为 self。非 ASCII 输入的解码条件与 xLua lua_tostring 保持一致；解码失败不更新短字符串缓存。

## 项目安装与配置

业务项目负责创建 XLuaFastCall Lua 表及 _RegisterType、InstallProxyMethods，管理启动开关、Proxy 生命周期和安装时机。框架不提供业务安装脚本或示例回调。

当某类型全部方法组都被跳过，其生成 FastCall 表和 Lua 安装列表可能为空。业务安装函数须把空列表作为无操作处理，在修改原方法表之前执行：

```lua
if count == 0 then
    return
end
```

已有完整生成文件但全部跳过时允许初始化；没有生成实现时仍要求先运行 Generate Code。业务代码仍从原 Wrap 调用被跳过的方法。修改后须在业务工程重新生成 C# 回调和 Lua 安装列表，不能继续使用旧注册产物。

通用项目配置通过 XLuaFastCallGenerator.Configure 提供目标类型、源码文件、条件宏和 Lua 输出目录。自动标签工具按精确签名匹配，支持多个重载；合并配置的所有 partial 文件后统一检查整组完整性。详见 [Editor/GENERATION-CONFIG.md](Editor/GENERATION-CONFIG.md)。

## ABI 与发布

生产 ABI 为 **1009**。Slot 16 字节、Cell 24 字节；Frame 保持 Receiver、Cells、Error、Count、Reserved，Count 包含 self，Reserved 显式补齐。64 位 Frame 为 32 字节、32 位为 24 字节。生产 Context/Frame 不包含 Token、验证计数或对象/Table 参数协议。

sd_layout(int*) 始终写 12 项，历史 Token 索引保留为零；版本、结构大小和字段偏移共同校验。更新版本号不会替代布局校验。

- sd_open(L)：保留生产初始化签名。
- sd_install(L,name,callback,kinds,count,required,handleField,strictName)：保留旧导出及对应托管 Register 重载；非严格模式仍要求精确参数数量。
- sd_install_proxy(L,name,callback,kinds,count,required,allowExtra,handleField,strictName)：生成的单分支入口，支持缺参/多参及 userdata self 限制。
- sd_install_group(L,name,callbacks,counts,arities,kinds,branches,handleField,strictName)：复制有序分支描述；kinds 每分支固定 32 项步长，最多 1024 分支。生成的静态委托数组负责回调保活。

生产 C 源、NativeAbi、Binding、生成器、业务生成产物及对应平台原生库须成套更新。使用本仓库的 build/CMakeLists.txt，启用 XLUA_FASTCALL=ON 并选择 Lua 5.3.5。更换库后重启已加载旧库的编辑器/进程。先 Dispose Binding，再 Dispose LuaEnv。

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
