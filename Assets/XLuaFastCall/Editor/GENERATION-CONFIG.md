# 项目生成配置

通用框架不依赖业务 Proxy、业务源码位置、条件宏或 Lua 输出路径。没有项目配置时，XLua/Generate Code 保持原行为：读取已有 `[XLua.XLuaFastCall]` 标签，只生成 C# 回调，不改业务标签、不输出 Lua 安装列表。

项目在自己的 Editor 目录注册配置。例如（MyProxy 和路径由项目替换）：

```csharp
using UnityEditor;
using XLuaFastCallEditor;

[InitializeOnLoad]
static class ProjectFastCallConfig
{
    static ProjectFastCallConfig()
    {
        XLuaFastCallGenerator.Configure += options =>
        {
            options.AutoTagTargets.Add(new XLuaFastCallAutoTagTarget
            {
                Enabled = true, // 默认为 false，必须显式开启源码修改
                TargetType = typeof(MyProxy),
                SourcePath = "Assets/Scripts/MyProxy.cs",
                ConditionSymbol = null // 例如 CLIENT；null 表示标签不包条件宏
            });
            options.LuaOutputDirectory = "Assets/StreamingAssets/Lua/FastCall";
        };
    }
}
```

对应类型仍需放在 XLua 的 `[LuaCallCSharp]` 配置中，原 Wrap 仍由 XLua 生成。注册事件只应填写 options，不应自己执行生成。`[InitializeOnLoad]` 确保域重载后重新注册配置。项目工具也可显式调用 `GenerateWithOptions(options)`；此入口只生成 FastCall 产物，不替代原 Wrap 的 GenAll。

## 自动标签

- 只有 `Enabled=true` 的目标会读取和改写业务源码；没有启用的目标不要求类型或路径有效。
- 使用通用 `UnsupportedReason` 规则筛选方法：当前支持的基本类型、默认参数、32 输入/16 输出等约束只有一个来源。
- SourcePath 与 AdditionalSourcePaths 中所有文件内的方法合并为该类型的生成集合，替换其旧标签集合；不自动扫描未配置的 partial 文件。每个类型及文件只能配置一次，每份文件中必须恰好有一个目标类型声明；同一方法匹配到多个文件时拒绝生成。
- 根据反射的精确参数签名匹配源码，不依赖刚写入、尚未重编译的标签。同一次生成使用相同方法集合输出 C# 回调和 Lua 表。
- 支持默认值中的括号、逗号、字符串、`default(T)`、转义标识符及参数属性；默认表达式只解析边界，不求值。默认值最终仍由 C# 编译器补齐。
- 只识别支持类型的标准 C# 别名及 System 类型名；不解析自定义 using 类型别名。方法签名、类型或默认值修改后先等待 Unity 成功编译，再生成。
- 源码解析限定在配置的命名空间/类型体内，保留其他类和其他属性。重复生成保持幂等；保留 UTF-8 BOM 和 LF/CRLF。非 UTF-8 源码会拒绝写回。
- 同名多个合格重载按精确签名分别选择；同一个 MethodInfo 匹配到多个源码条件分支时排除该签名。合并所有配置文件后，只有完整方法组全部被选择且支持，才生成 FastCall，否则整组保留原 Wrap，并在 generation-decisions.txt 记录原因。词法解析不执行预处理器；条件分支导致整体括号不配对、目标声明不唯一时明确失败，不猜测改写。
- 自动工具生成 `[XLuaFastCall]`。缺少适用的文件级 `using XLua;` 时自动补充；已有 `using global::XLua;` 也可复用。配置了条件宏时，新补的 using 与标签使用同一条件；已有引用保持原样。`ConditionSymbol` 只接受单个 C# 标识符，不接受表达式或代码；该宏是否启用由项目编译配置负责。
- 所有配置和生成方法校验成功后才写业务文件，并检查源文件是否在准备期间变化。文件系统写入阶段不提供跨多个文件的事务回滚。
- 关闭自动改标签不会删除已经存在的标签，也不等于关闭 FastCall；此时生成器按已有标签工作。

如仅需要预览，可使用 `XLuaFastCallAutoTags.RebuildSource(type, source, conditionSymbol, out methods)`。该方法返回新源码及精确 MethodInfo 集合，不执行文件写入。

## Lua 安装列表

`LuaOutputDirectory=null` 为关闭；提供目录后，每个参与 FastCall 选择的类型输出 `<TypeName>XLuaFastCall.lua`：

```lua
local fast = XLuaFastCall["MyProxy"]
return {
    ["GetLevelEnterType"] = fast["GetLevelEnterType"],
}
```

使用方括号字符串键，支持名称与 Lua 关键字重合的 C# 方法。表中直接引用原生闭包，不增加 Lua 包装函数，也不包含业务实现。

全部方法组被跳过时，生成的表可能为空；安装函数须将空列表作为无操作，不报错也不替换原入口。

由项目现有安装函数加载这张表并替换原 Wrap 入口；框架不决定何时安装，也不创建 LuaEnv。Client 的启动开关、Proxy 生命周期及安装函数仍属于项目代码。

启用 Lua 输出时只清理该目录内带本生成器专属文件头的过期 `*XLuaFastCall.lua`，不删除其他手写 Lua 文件。关闭输出时不动已有 Lua 文件。`EmitLua(type, methods)` 也可单独用于预览文本。

## Client 后续接入

Client 可将配置设为 XFightScriptProxy、共享的 Funcs.cs、CLIENT 条件宏及 XBuilder.ProductPath 下的 Lua/DlcFight/Gen。届时移除 Client 生成器中的同类私有实现，改为项目配置调用通用能力；这些业务配置不放回框架。

框架同步不会替业务工程生成回调或迁移业务脚本。升级到生产 ABI 1009 后，业务工程须重新生成绑定、处理空安装列表，并发布匹配的平台原生库。
