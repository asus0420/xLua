using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using CSObjectWrapEditor;
using UnityEngine;

namespace XLuaFastCallEditor
{
    public static class XLuaFastCallGenerator
    {
        public static event Action<XLuaFastCallGenerationOptions> Configure;

        const string Header = "// 由 XLuaFastCallEditor.XLuaFastCallGenerator 自动生成，请勿手动修改。\n";
        const string ClassHeader = "using System;\n\nnamespace XLuaFastCall\n{\n    public sealed partial class Binding\n    {\n";

        static string Q(string s) => "\"" + s.Replace("\\", "\\\\").Replace("\"", "\\\"").Replace("\r", "\\r").Replace("\n", "\\n") + "\"";

        static string T(Type t) => "global::" + t.FullName.Replace('+', '.');

        static Type ValueType(ParameterInfo p) => p.ParameterType.IsByRef ? p.ParameterType.GetElementType() : p.ParameterType;

        static bool Supported(Type t) => new[] { typeof(int), typeof(long), typeof(float), typeof(double), typeof(bool), typeof(string), typeof(byte) }.Contains(t);

        // 只收集方法本身的标签，直接保留精确签名，不按方法名重新查找重载。
        static Dictionary<Type, MethodInfo[]> CollectMethods()
        {
            var config = new Dictionary<Type, MethodInfo[]>();

            foreach (var type in XLua.Utils.GetAllTypes())
            {
                var methods = type.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly)
                    .Where(m => m.IsDefined(typeof(XLua.XLuaFastCallAttribute), false)).ToArray();

                if (methods.Length > 0)
                    config.Add(type, methods);
            }

            return config;
        }

        public static string UnsupportedReason(MethodInfo m)
        {
            if (!m.IsPublic)
                return "方法不是 public，FastCall 只支持公开实例方法";

            if (m.IsGenericMethod)
                return "不支持泛型方法";

            if (m.IsSpecialName)
                return "不支持特殊名称方法（属性访问器、运算符等）";

            if (m.IsStatic)
                return "不支持静态方法，FastCall 只支持实例方法";

            if (m.IsDefined(typeof(XLua.BlackListAttribute), true))
                return "方法带有 XLua BlackList 特性，不能导出";

            var ps = m.GetParameters();
            foreach (var p in ps)
            {
                if (p.IsDefined(typeof(ParamArrayAttribute), false))
                    return "参数 " + p.Name + " 使用 params，不支持可变参数";

                if (p.IsIn && p.ParameterType.IsByRef)
                    return "参数 " + p.Name + " 使用 in，不支持只读引用参数";

                if (p.IsOptional && p.ParameterType.IsByRef)
                    return "参数 " + p.Name + " 是可选引用参数，不支持此声明";

                if (p.IsOptional && !p.HasDefaultValue)
                    return "可选参数 " + p.Name + " 缺少默认值";
            }

            bool optionalSeen = false;
            foreach (var p in ps.Where(p => !p.IsOut))
            {
                if (p.IsOptional)
                    optionalSeen = true;
                else if (optionalSeen)
                    return "必填参数 " + p.Name + " 位于可选参数之后";
            }

            const string supportedTypes = "仅支持 int、byte、long、float、double、bool、string；Vector 和枚举请使用显式数值接口";
            if (m.ReturnType != typeof(void) && !Supported(m.ReturnType))
                return "返回值类型 " + m.ReturnType + " 不支持；" + supportedTypes;

            var unsupportedParameters = ps.Where(p => !Supported(ValueType(p))).ToArray();
            if (unsupportedParameters.Length > 0)
                return "参数类型不支持：" + string.Join("；", unsupportedParameters.Select(p => p.Name + " (" + p.ParameterType + ")")) + "；" + supportedTypes;

            int inputs = ps.Count(p => !p.IsOut);
            if (inputs > XLuaFastCall.Binding.MaxInputs)
                return "输入槽位数 " + inputs + " 超过上限 " + XLuaFastCall.Binding.MaxInputs;

            int outputs = (m.ReturnType == typeof(void) ? 0 : 1) + ps.Count(p => p.ParameterType.IsByRef);
            if (outputs > XLuaFastCall.Binding.MaxOutputs)
                return "输出槽位数 " + outputs + " 超过上限 " + XLuaFastCall.Binding.MaxOutputs + "（返回值及 ref/out 参数均计入）";

            return null;
        }

        static int Kind(Type t, bool permissive) =>
            t == typeof(string) ? 7 :
            t == typeof(bool) ? 3 :
            (t == typeof(float) || t == typeof(double)) ? (permissive ? 2 : 10) : 1;

        static void ValidateMethod(MethodInfo method)
        {
            string reason = UnsupportedReason(method);
            if (reason != null)
                throw new InvalidOperationException("XLuaFastCall: attributed method " + method.DeclaringType.FullName + "." + method + " cannot be generated: " + reason);
        }

        static string Read(Type t, int i, bool permissive)
        {
            if (t == typeof(string))
                return "c.String(" + i + ")";

            if (t == typeof(bool))
                return "c.Boolean(" + i + ")";

            if (t == typeof(float))
                return permissive ? "(float)c.Number(" + i + ")" : "c.Float(" + i + ")";

            if (t == typeof(double))
                return "c." + (permissive ? "Number" : "StrictNumber") + "(" + i + ")";

            return "(" + T(t) + ")" + "c.Integer(" + i + ")";
        }

        static string Emit(Type type, MethodInfo m, string id, bool permissive)
        {
            var b = new StringBuilder();
            var parameters = m.GetParameters();
            var inputParams = parameters.Where(p => !p.IsOut).ToArray();
            int required = inputParams.Count(p => !p.IsOptional);
            bool strict = required != inputParams.Length;

            b.AppendLine("        [AOT.MonoPInvokeCallback(typeof(Callback))]");
            b.AppendLine("        static int " + id + "(IntPtr L, IntPtr args, IntPtr frame)");
            b.AppendLine("        {");
            b.AppendLine("            var c = new CallFrame(args, frame);");
            b.AppendLine("            try");
            b.AppendLine("            {");
            b.AppendLine("                var target = global::XLuaFastCall.ProxyRegistry.Get<" + T(type) + ">(c.Receiver);");
            if (strict)
                b.AppendLine("                int argc = c.ArgCount;");

            var args = new List<string>();
            var results = new List<KeyValuePair<Type, string>>();
            int input = 0;

            if (m.ReturnType != typeof(void))
                results.Add(new KeyValuePair<Type, string>(m.ReturnType, "result"));

            foreach (var p in parameters)
            {
                var t = ValueType(p);
                string n = "p" + p.Position;
                string read = p.IsOut ? null : Read(t, input, permissive);

                if (p.IsOptional)
                    read = "argc > " + input + " ? " + read + " : default";

                if (!p.IsOut)
                    input++;

                b.AppendLine("                " + T(t) + " " + n + (p.IsOut ? ";" : " = " + read + ";"));
                args.Add((p.IsOut ? "out " : p.ParameterType.IsByRef ? "ref " : "") + n);

                if (p.ParameterType.IsByRef)
                    results.Add(new KeyValuePair<Type, string>(t, n));
            }

            b.AppendLine();
            if (!strict)
                b.AppendLine("                " + (m.ReturnType == typeof(void) ? "" : "var result = ") + "target.@" + m.Name + "(" + string.Join(", ", args) + ");");
            else
            {
                if (m.ReturnType != typeof(void))
                    b.AppendLine("                " + T(m.ReturnType) + " result;");

                b.AppendLine("                switch (argc)");
                b.AppendLine("                {");
                for (int argc = required; argc <= inputParams.Length; argc++)
                {
                    int slot = 0;
                    var passed = new List<string>();
                    foreach (var p in parameters)
                    {
                        if (p.IsOut || slot++ < argc)
                            passed.Add(args[p.Position]);
                    }

                    // 省略可选参数，由 C# 提供默认值，无需将默认值序列化为字面量。
                    b.AppendLine("                    " + (argc == inputParams.Length ? "default" : "case " + argc) + ": " +
                        (m.ReturnType == typeof(void) ? "" : "result = ") + "target.@" + m.Name + "(" + string.Join(", ", passed) + "); break;");
                }

                b.AppendLine("                }");
            }

            b.AppendLine();
            bool strings = results.Any(r => r.Key == typeof(string));
            bool singleString = strings && results.Count == 1;

            if (strings)
                b.AppendLine("                int used = 0;");

            if (strings && !singleString)
                b.AppendLine("                bool fits = true;");

            for (int i = 0; i < results.Count; i++)
            {
                var r = results[i];
                if (singleString)
                {
                    b.AppendLine("                if (!c.ReturnString(0, " + r.Value + ", ref used))");
                    b.AppendLine("                {");
                    b.AppendLine("                    LuaStringCodec.Push(L, " + r.Value + ");");
                    b.AppendLine("                    return 101;");
                    b.AppendLine("                }");
                }
                else
                    b.AppendLine("                " + (r.Key == typeof(string) ? "fits &= c.ReturnString(" + i + ", " + r.Value + ", ref used);" : "c.Return(" + i + ", " + r.Value + ");"));
            }

            if (strings && !singleString)
            {
                b.AppendLine("                if (!fits)");
                b.AppendLine("                {");
                foreach (var r in results)
                    b.AppendLine("                    PushConfigured(L, " + r.Value + ");");
                b.AppendLine("                    return " + (100 + results.Count) + ";");
                b.AppendLine("                }");
            }

            b.AppendLine("                return " + results.Count + ";");
            b.AppendLine("            }");
            b.AppendLine("            catch (Exception e)");
            b.AppendLine("            {");
            b.AppendLine("                return c.Error(e);");
            b.AppendLine("            }");
            b.AppendLine("        }");
            b.AppendLine();
            return b.ToString();
        }

        [GenCodeMenu]
        public static void Generate()
        {
            var options = new XLuaFastCallGenerationOptions();
            Configure?.Invoke(options);
            GenerateWithOptions(options);
        }

        // 供项目工具显式调用的入口，统一校验代码生成与源码修改。
        public static void GenerateWithOptions(XLuaFastCallGenerationOptions options)
        {
            if (options == null)
                throw new ArgumentNullException(nameof(options));

            if (Generator.LuaCallCSharp == null)
                Generator.GetGenConfig(XLua.Utils.GetAllTypes());

            var config = CollectMethods();
            var sourceEdits = new List<AutoTagWrite>();
            var targetTypes = new HashSet<Type>();
            var sourcePaths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

            foreach (var target in options.AutoTagTargets)
            {
                if (target == null)
                    throw new ArgumentException("Null auto-tag target");

                if (!target.Enabled)
                    continue;

                if (UnityEditor.EditorApplication.isCompiling || UnityEditor.EditorUtility.scriptCompilationFailed)
                    throw new InvalidOperationException("Wait for successful script compilation before rebuilding FastCall tags");

                if (target.TargetType == null || !targetTypes.Add(target.TargetType))
                    throw new ArgumentException("Missing or duplicate auto-tag target type");

                var targetMethods = new List<MethodInfo>();
                foreach (var sourcePath in new[] { target.SourcePath }.Concat(target.AdditionalSourcePaths))
                {
                    if (string.IsNullOrWhiteSpace(sourcePath))
                        throw new ArgumentException("Auto-tag SourcePath is required");

                    string path = Path.GetFullPath(sourcePath);
                    if (!sourcePaths.Add(path))
                        throw new ArgumentException("Each auto-tag source file may be configured only once");

                    byte[] original = File.ReadAllBytes(path);
                    bool hasBom = original.Length >= 3 && original[0] == 239 && original[1] == 187 && original[2] == 191;
                    string source = new UTF8Encoding(false, true).GetString(original, hasBom ? 3 : 0, original.Length - (hasBom ? 3 : 0));
                    MethodInfo[] selected;
                    string rebuilt = XLuaFastCallAutoTags.RebuildSource(target.TargetType, source, target.ConditionSymbol, out selected);
                    sourceEdits.Add(new AutoTagWrite { Path = path, Original = original, Source = source, Rebuilt = rebuilt });
                    targetMethods.AddRange(selected);
                }

                if (targetMethods.Distinct().Count() != targetMethods.Count)
                    throw new ArgumentException("Auto-tag method matched more than one configured source file: " + target.TargetType);

                // 域重载前，反射读取的标签可能尚未更新。
                // 回调和 Lua 代码均使用本次精确选中的方法。
                if (targetMethods.Count == 0)
                    config.Remove(target.TargetType);
                else
                    config[target.TargetType] = targetMethods.ToArray();
            }

            var decisions = new List<string>();
            var files = new Dictionary<string, string>();
            var names = new HashSet<string>();
            var luaFiles = new Dictionary<string, string>();
            var install = new StringBuilder(Header + ClassHeader + "        partial void InstallConfigured(ref int installed)\n        {\n");
            int typeIndex = 0;

            foreach (var entry in config.OrderBy(k => k.Key.FullName, StringComparer.Ordinal))
            {
                var type = entry.Key;
                if (Generator.LuaCallCSharp == null || !Generator.LuaCallCSharp.Contains(type))
                    throw new InvalidOperationException("XLuaFastCall type must also be in XLua LuaCallCSharp configuration: " + type);

                if (!type.IsClass || !type.IsVisible || type.ContainsGenericParameters || !names.Add(type.Name) ||
                    type.Name.StartsWith("_") || type.Name == "Enums" || type.Name == "InstallProxyMethods")
                    throw new InvalidOperationException("Invalid or duplicate XLuaFastCall type name: " + type);

                var selected = new HashSet<MethodInfo>(entry.Value);
                var allGroups = XLuaMethodGroups.Read(type);
                var groups = new List<XLuaMethodGroups.Group>();

                foreach (var group in allGroups.Where(g => g.Methods.Any(selected.Contains)))
                {
                    var missing = group.Methods.Where(m => !selected.Contains(m)).ToArray();
                    if (missing.Length > 0)
                    {
                        throw new InvalidOperationException(
                            "XLuaFastCall 生成失败：同名的重载函数必须保持一致，要么全部添加 [XLuaFastCall] 特性，要么全部不添加。\n" +
                            "重载函数：" + type.FullName + "." + group.Name);
                    }

                    var unsupported = group.Methods.Select(m => new { Method = m, Reason = UnsupportedReason(m) }).Where(x => x.Reason != null).ToArray();
                    if (group.IsStatic || unsupported.Length > 0 || group.Branches.Count > 1024)
                    {
                        throw new InvalidOperationException(
                            "XLuaFastCall 生成失败：已添加 [XLuaFastCall] 的重载组不满足生成条件。\n" +
                            "重载组：" + type.FullName + "." + group.Name + "\n" +
                            (group.IsStatic ? "原因：不支持静态方法组。\n" : "") +
                            (group.Branches.Count > 1024 ? "原因：重载分支数 " + group.Branches.Count + " 超过上限 1024（包含可选参数展开的分支）。\n" : "") +
                            "该组方法：\n" + string.Join("\n", group.Methods.Select(m => "  " + m.DeclaringType.FullName + "." + m)) + "\n" +
                            string.Join("\n", unsupported.Select(x => "不支持的方法：" + x.Method.DeclaringType.FullName + "." + x.Method + "；原因：" + x.Reason)) + "\n" +
                            "请修正上述声明，或移除该组所有 [XLuaFastCall] 特性以使用普通 XLua Wrap。");
                    }

                    groups.Add(group);
                    decisions.Add("FAST " + type.FullName + "." + group.Name + " | branches=" + group.Branches.Count);
                }

                var notExported = selected.Where(m => !allGroups.Any(g => g.Methods.Contains(m))).ToArray();
                if (notExported.Length > 0)
                {
                    throw new InvalidOperationException(
                        "XLuaFastCall 生成失败：以下方法已添加 [XLuaFastCall]，但未进入 XLua 导出方法组，无法生成 FastCall。\n" +
                        string.Join("\n", notExported.Select(m => "  " + m.DeclaringType.FullName + "." + m + "；原因：" +
                            (UnsupportedReason(m) ?? "被 XLua 导出规则过滤，请检查 BlackList 配置及方法导出配置"))) + "\n" +
                        "请修正导出配置或方法声明，或移除对应的 [XLuaFastCall] 特性。");
                }

                var methods = groups.SelectMany(g => g.Methods).Distinct().ToList();
                if (options.LuaOutputDirectory != null)
                    luaFiles.Add(type.Name + "XLuaFastCall.lua", EmitLua(type, methods));

                var b = new StringBuilder(Header + ClassHeader);
                var ids = new List<string>();
                for (int i = 0; i < methods.Count; i++)
                {
                    string id = "Configured_" + typeIndex + "_" + i;
                    ids.Add(id);
                    bool permissive = groups.Single(g => g.Methods.Contains(methods[i])).Branches.Count == 1;
                    b.Append(Emit(type, methods[i], id, permissive));
                }

                // 注册时必须使用这些委托实例，Mono 下也保持一致。
                b.AppendLine("        static readonly Callback[] ConfiguredRoots" + typeIndex + " =");
                b.AppendLine("        {");
                foreach (var id in ids)
                    b.AppendLine("            " + id + ",");
                b.AppendLine("        };\n    }\n}");
                files.Add(type.Name + "XLuaFastCall.cs", b.ToString());

                install.AppendLine("            BeginConfiguredType(typeof(" + T(type) + "), " + Q(type.Name) + ", new string[]");
                install.AppendLine("            {");
                foreach (var group in groups)
                    install.AppendLine("                " + Q(group.Name) + ",");
                install.AppendLine("            });\n            try\n            {");
                foreach (var group in groups)
                {
                    if (group.Branches.Count == 1)
                    {
                        var m = group.Methods[0];
                        var inputs = m.GetParameters().Where(p => !p.IsOut).ToArray();
                        install.AppendLine("                Register(" + Q(group.Name) + ", ConfiguredRoots" + typeIndex + "[" + methods.IndexOf(m) + "], new int[] { " +
                            string.Join(", ", inputs.Select(p => Kind(ValueType(p), true).ToString())) + " }, 0, true, userdataOnly: true);");
                    }
                    else
                    {
                        install.AppendLine("                RegisterGroup(" + Q(group.Name) + ", new Callback[] { " +
                            string.Join(", ", group.Branches.Select(branch => "ConfiguredRoots" + typeIndex + "[" + methods.IndexOf(branch.Method) + "]")) + " },");
                        install.AppendLine("                    new int[][] { " + string.Join(", ", group.Branches.Select(branch => "new int[] { " +
                            string.Join(", ", branch.Method.GetParameters().Where(p => !p.IsOut).Select(p => Kind(ValueType(p), false).ToString())) + " }")) + " },");
                        install.AppendLine("                    new int[] { " + string.Join(", ", group.Branches.Select(branch => branch.Arity.ToString())) + " }, " +
                            Q(type.FullName.Replace('+', '.') + "." + group.Methods[0].Name) + ");");
                    }

                    install.AppendLine("                installed++;");
                }

                install.AppendLine("            }\n            finally\n            {\n                EndConfiguredType();\n            }\n");
                typeIndex++;
            }

            install.AppendLine("        }\n        partial void ConfiguredGenerationPresent(ref bool present) { present = true; }\n    }\n}");
            files.Add("XLuaFastCallRegister.Generated.cs", install.ToString());

            string luaOutput = null;
            if (options.LuaOutputDirectory != null)
            {
                if (string.IsNullOrWhiteSpace(options.LuaOutputDirectory))
                    throw new ArgumentException("LuaOutputDirectory must be null (disabled) or a directory");

                luaOutput = Path.GetFullPath(options.LuaOutputDirectory);
            }

            // 修改业务源码前，先完成所有配置类型的校验。
            foreach (var edit in sourceEdits)
                if (!File.ReadAllBytes(edit.Path).SequenceEqual(edit.Original))
                    throw new IOException("Auto-tag source changed during generation: " + edit.Path);

            string output = Path.Combine(GeneratorConfig.common_path, "XLuaFastCall");
            Directory.CreateDirectory(output);
            if (luaOutput != null)
                Directory.CreateDirectory(luaOutput);

            foreach (var edit in sourceEdits)
            {
                if (edit.Source == edit.Rebuilt)
                    continue;

                bool bom = edit.Original.Length >= 3 && edit.Original[0] == 239 && edit.Original[1] == 187 && edit.Original[2] == 191;
                File.WriteAllText(edit.Path, edit.Rebuilt, new UTF8Encoding(bom));
            }

            foreach (var file in Directory.GetFiles(output, "*.cs"))
                if (!files.ContainsKey(Path.GetFileName(file)) && (File.ReadAllText(file).StartsWith(Header, StringComparison.Ordinal) ||
                    File.ReadAllText(file).StartsWith("// Generated by XLuaFastCallEditor.XLuaFastCallGenerator.", StringComparison.Ordinal)))
                    File.Delete(file);

            foreach (var file in files)
                File.WriteAllText(Path.Combine(output, file.Key), file.Value, new UTF8Encoding(false));

            if (luaOutput != null)
            {
                foreach (var file in Directory.GetFiles(luaOutput, "*XLuaFastCall.lua"))
                    if (!luaFiles.ContainsKey(Path.GetFileName(file)) && File.ReadAllText(file).StartsWith(LuaHeader, StringComparison.Ordinal))
                        File.Delete(file);

                foreach (var file in luaFiles)
                    File.WriteAllText(Path.Combine(luaOutput, file.Key), file.Value, new UTF8Encoding(false));
            }

            File.WriteAllLines(Path.Combine(output, "generation-decisions.txt"), decisions, new UTF8Encoding(false));
            Debug.Log("XLuaFastCall Generate finished: " + output + "; generated groups: " + decisions.Count);
        }

        const string LuaHeader = "-- Generated by XLuaFastCallEditor.XLuaFastCallGenerator.\n";

        static string LuaQuote(string value) => "\"" + value.Replace("\\", "\\\\").Replace("\"", "\\\"").Replace("\r", "\\r").Replace("\n", "\\n") + "\"";

        public static string EmitLua(Type type, IEnumerable<MethodInfo> methods)
        {
            if (type == null || methods == null)
                throw new ArgumentNullException();

            var b = new StringBuilder(LuaHeader);
            b.AppendLine("return function(fast)");
            b.AppendLine("    return {");
            foreach (var method in methods.GroupBy(m => m.Name).Select(g => g.First()).OrderBy(m => m.Name, StringComparer.Ordinal))
                b.AppendLine("        [" + LuaQuote(method.Name) + "] = fast[" + LuaQuote(method.Name) + "],");
            b.AppendLine("    }");
            b.AppendLine("end");
            return b.ToString();
        }

        sealed class AutoTagWrite
        {
            public string Path, Source, Rebuilt;
            public byte[] Original;
        }
    }
}
