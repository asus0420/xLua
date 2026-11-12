# XLuaFastCall

方案 A：C 使用与 Lua 核心一起编译的内部宏读取参数，写入固定宽度 Slot；C# 专属回调读取 Slot、按句柄调用业务方法，再由 C 返回结果。数字与字符串的兼容转换保留原有行为。

Lua 内部布局仅在 C 中使用；C# 不读取 TValue 或 TString。当前要求 Lua 5.3.5，非 LuaJIT、小端序，并保留原有 Lua 数值配置。

## 目录

- Runtime/Binding.cs：生命周期、共享结构校验、参数读取及结果写入。
- Runtime/Binding.Configured.cs：生成回调的注册支持。
- Runtime/LuaStringCodec.cs：按内容地址和长度读取字符串，短字符串缓存、UTF-8 编解码及返回缓冲。
- Runtime/ProxyRegistry.cs：对象句柄分配、查询及释放。
- Editor/XLuaFastCallAttribute.cs：优化方法配置标记。
- Editor/XLuaFastCallGenerator.cs：接入 XLua GenCodeMenu，生成专属回调。
- 仓库根目录 build/luaextension/XLuaFastCall/bridge.c：与 Lua 核心编入同一个原生库。

运行时、生成回调与 XLua 源码需在同一程序集。Assets/csc.rsp 启用 unsafe，不单独建立 asmdef。

## ABI 与生命周期

自定义布局版本为 1002。Slot 固定 16 字节、Cell 固定 32 字节，地址使用 uint64_t / ulong；Frame 的 Receiver 位于首字段，32 位为 24 字节、64 位为 40 字节。编译期断言和初始化时的大小、字段偏移校验共同检查布局。

正式接口保持 `new Binding(env)` 和两参数 `sd_open(L, token)`，不包含 SD_VERIFY、计数器、验证入口或 sd_calls。TestLua 带计数版本的原生库不能直接替代本仓库的库，需使用本仓库配套构建产物。

旧布局版本的库不能与更新后的 C# 混用。更换原生库后需要重启已加载该库的编辑器或进程。销毁时先 Dispose Binding，再 Dispose LuaEnv。

## 构建

沿用本仓库 build 目录中的平台脚本，不迁入 TestLua 的 Tools 或独立构建模块。CMake 的 XLUA_FASTCALL 总开关默认 OFF，由以下 Lua 5.3 构建入口显式启用：

| 脚本 | 优化架构 | 分发路径 |
|---|---|---|
| make_win64_lua53.bat | x64 | build/plugin_lua53/Plugins/x86_64/xlua.dll |
| make_win32_lua53.bat | x86 | build/plugin_lua53/Plugins/x86/xlua.dll |
| make_android_lua53.sh | armeabi-v7a、arm64-v8a、x86 | build/plugin_lua53/Plugins/Android/libs/对应架构/libxlua.so |
| make_ios_lua53.sh | arm64 | build/plugin_lua53/Plugins/iOS/libxlua.a |

Android 沿用原 NDK、API 等配置；iOS 需 macOS 和 Xcode。Windows/Android 的 C# 入口使用 xlua，iOS 使用 __Internal。不生成单独的优化动态库，不携带测试计数开关。

## 项目接入边界

业务项目负责原 XLua 的 LuaCallCSharp 配置、优化方法名单、Lua 安装逻辑和启动开关，以及对象句柄的生命周期。本目录不包含测试用例、示例业务类型、生成的示例回调、Session 或业务配置。

生成器输出到 GeneratorConfig.common_path/XLuaFastCall/。同名重载或不支持的签名在 Console 报 Error，不生成该方法的优化入口。枚举常量由原 XLua 导出，优化回调不增加 long/ulong 枚举或名称解析功能。

初始化 Binding 前，项目创建 XLuaFastCall Lua 表并提供 _RegisterType 安装接口，参数依次为原 userdata 元表、类型名、fast 表、方法名列表。关闭优化时不创建 Binding，继续使用原 Wrap。

## 验证范围

方案 A 在 TestLua 已完成 Windows x64 的生成、IL2CPP Release 构建、正确性及性能测试，以及 Windows x86 原生库和跨语言 ABI 冒烟验证。这些结果不等于本仓库及业务项目的所有平台验证。

本仓库于 2026-11-12 完成以下迁移验证：

- 使用本工程 Unity 响应文件独立编译运行时和 Editor C#，均通过。
- Windows x64、x86 原生 Release 构建通过，输出在 Temp/PlanAMigration/native-x64、native-x86。
- 在对应位宽进程中验证 ABI 1002、超过 32 位范围的句柄、混合参数、四返回值、UTF-8 和重复上下文拒绝，均通过。
- 确认正式库不导出 sd_calls；Android/iOS 脚本语法检查通过。

这些检查未运行本仓库业务回调的完整正确性或性能测试。Android/iOS 尚未构建及真机验证。已有分发目录和 Assets/Plugins 中的旧二进制没有被替换，使用前必须重新构建并部署，不能混用旧库。
