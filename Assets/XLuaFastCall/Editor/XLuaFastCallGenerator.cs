using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using CSObjectWrapEditor;
using UnityEngine;

namespace XLuaFastCallEditor {
    public static class XLuaFastCallGenerator {
        public static event Action<XLuaFastCallGenerationOptions> Configure;
        const string Header = "// 由 XLuaFastCallEditor.XLuaFastCallGenerator 自动生成，请勿手动修改。\n";
        const string ClassHeader = "using System;\n\nnamespace XLuaFastCall\n{\n    public sealed partial class Binding\n    {\n";
        static string Q(string s) => "\""+s.Replace("\\","\\\\").Replace("\"","\\\"").Replace("\r","\\r").Replace("\n","\\n")+"\"";
        static string T(Type t) => "global::"+t.FullName.Replace('+','.');
        static Type ValueType(ParameterInfo p) => p.ParameterType.IsByRef ? p.ParameterType.GetElementType() : p.ParameterType;
        static bool Supported(Type t) => new[]{typeof(int),typeof(long),typeof(float),typeof(double),typeof(bool),typeof(string),typeof(byte)}.Contains(t);
        // 只收集方法本身的标签，直接保留精确签名，不按方法名重新查找重载。
        static Dictionary<Type, MethodInfo[]> CollectMethods() {
            var config = new Dictionary<Type, MethodInfo[]>();
            foreach (var type in XLua.Utils.GetAllTypes()) {
                var methods = type.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly)
                    .Where(m => m.IsDefined(typeof(XLua.XLuaFastCallAttribute), false)).ToArray();
                if (methods.Length > 0) config.Add(type, methods);
            }
            return config;
        }
        public static string UnsupportedReason(MethodInfo m) {
            if(!m.IsPublic || m.IsGenericMethod || m.IsSpecialName || m.IsStatic)return "non-public/generic/static/special method";
            if(m.IsDefined(typeof(XLua.BlackListAttribute),true))return "XLua BlackList";
            var ps=m.GetParameters();
            if(ps.Any(p=>p.IsDefined(typeof(ParamArrayAttribute),false)))return "params parameters";
            if(ps.Any(p=>p.IsIn && p.ParameterType.IsByRef))return "in parameters";
            if(ps.Any(p=>p.IsOptional && p.ParameterType.IsByRef))return "optional byref parameter";
            if(ps.Any(p=>p.IsOptional && !p.HasDefaultValue))return "optional parameter without default value";
            bool optionalSeen=false;
            foreach(var p in ps.Where(p=>!p.IsOut)) {
                if(p.IsOptional)optionalSeen=true;
                else if(optionalSeen)return "required input after optional parameter";
            }
            if((m.ReturnType!=typeof(void)&&!Supported(m.ReturnType)) || ps.Any(p=>!Supported(ValueType(p))))return "unsupported type (only int, byte, long, float, double, bool, string; Vector and enum signatures must use explicit numeric interfaces)";
            if(ps.Count(p=>!p.IsOut)>XLuaFastCall.Binding.MaxInputs)return "more than "+XLuaFastCall.Binding.MaxInputs+" input slots";
            if((m.ReturnType==typeof(void)?0:1)+ps.Count(p=>p.ParameterType.IsByRef)>XLuaFastCall.Binding.MaxOutputs)return "more than "+XLuaFastCall.Binding.MaxOutputs+" output slots";
            return null;
        }
        static int Kind(Type t) => t==typeof(string)?7:t==typeof(bool)?3:(t==typeof(float)||t==typeof(double))?10:1;
        static void ValidateMethod(MethodInfo method) {
            string reason=UnsupportedReason(method);
            if(reason!=null)
                throw new InvalidOperationException("XLuaFastCall: attributed method "+method.DeclaringType.FullName+"."+method+" cannot be generated: "+reason);
        }
        static string Read(Type t,int i) {
            if(t==typeof(string))return "c.String("+i+")";
            if(t==typeof(bool))return "c.Boolean("+i+")";
            if(t==typeof(float))return "c.Float("+i+")";
            if(t==typeof(double))return "c.StrictNumber("+i+")";
            return "("+T(t)+")"+"c.Integer("+i+")";
        }

        static string Emit(Type type,MethodInfo m,string id) {
            var b=new StringBuilder();
            var parameters=m.GetParameters();
            var inputParams=parameters.Where(p=>!p.IsOut).ToArray();
            int required=inputParams.Count(p=>!p.IsOptional);
            bool strict=required!=inputParams.Length;
            b.AppendLine("        [AOT.MonoPInvokeCallback(typeof(Callback))]");
            b.AppendLine("        static int "+id+"(IntPtr L, IntPtr args, IntPtr frame)");
            b.AppendLine("        {");
            b.AppendLine("            var c = new CallFrame(args, frame);");
            b.AppendLine("            try");
            b.AppendLine("            {");
            b.AppendLine("                var target = global::XLuaFastCall.ProxyRegistry.Get<"+T(type)+">(c.Receiver);");
            if(strict)b.AppendLine("                int argc = c.ArgCount;");
            var args=new List<string>();var results=new List<KeyValuePair<Type,string>>();int input=0;
            if(m.ReturnType!=typeof(void))results.Add(new KeyValuePair<Type,string>(m.ReturnType,"result"));
            foreach(var p in parameters) {
                var t=ValueType(p);string n="p"+p.Position;
                string read=p.IsOut?null:Read(t,input);
                if(p.IsOptional)read="argc > "+input+" ? "+read+" : default";
                if(!p.IsOut)input++;
                b.AppendLine("                "+T(t)+" "+n+(p.IsOut?";":" = "+read+";"));
                args.Add((p.IsOut?"out ":p.ParameterType.IsByRef?"ref ":"")+n);
                if(p.ParameterType.IsByRef)results.Add(new KeyValuePair<Type,string>(t,n));
            }
            b.AppendLine();
            if(!strict)b.AppendLine("                "+(m.ReturnType==typeof(void)?"":"var result = ")+"target.@"+m.Name+"("+string.Join(", ",args)+");");
            else {
                if(m.ReturnType!=typeof(void))b.AppendLine("                "+T(m.ReturnType)+" result;");
                b.AppendLine("                switch (argc)");
                b.AppendLine("                {");
                for(int argc=required;argc<=inputParams.Length;argc++) {
                    int slot=0;
                    var passed=new List<string>();
                    foreach(var p in parameters) {
                        if(p.IsOut || slot++<argc)passed.Add(args[p.Position]);
                    }
                    // Omit optional arguments; C# supplies defaults without literal serialization.
                    b.AppendLine("                    "+(argc==inputParams.Length?"default":"case "+argc)+": "+(m.ReturnType==typeof(void)?"":"result = ")+"target.@"+m.Name+"("+string.Join(", ",passed)+"); break;");
                }
                b.AppendLine("                }");
            }
            b.AppendLine();
            bool strings=results.Any(r=>r.Key==typeof(string));
            bool singleString=strings && results.Count==1;
            if(strings)b.AppendLine("                int used = 0;");
            if(strings && !singleString)b.AppendLine("                bool fits = true;");
            for(int i=0;i<results.Count;i++) {
                var r=results[i];
                if(singleString) {
                    b.AppendLine("                if (!c.ReturnString(0, "+r.Value+", ref used))");
                    b.AppendLine("                {");
                    b.AppendLine("                    LuaStringCodec.Push(L, "+r.Value+");");
                    b.AppendLine("                    return 101;");
                    b.AppendLine("                }");
                }
                else b.AppendLine("                "+(r.Key==typeof(string)?"fits &= c.ReturnString("+i+", "+r.Value+", ref used);":"c.Return("+i+", "+r.Value+");"));
            }
            if(strings && !singleString) {
                b.AppendLine("                if (!fits)");
                b.AppendLine("                {");
                foreach(var r in results)b.AppendLine("                    PushConfigured(L, "+r.Value+");");
                b.AppendLine("                    return "+(100+results.Count)+";");
                b.AppendLine("                }");
            }
            b.AppendLine("                return "+results.Count+";");
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
        public static void Generate() {
            var options = new XLuaFastCallGenerationOptions();
            Configure?.Invoke(options);
            GenerateWithOptions(options);
        }

        // Explicit entry point for project tools. No native code or runtime ABI changes.
        public static void GenerateWithOptions(XLuaFastCallGenerationOptions options) {
            if(options==null)throw new ArgumentNullException(nameof(options));
            if(Generator.LuaCallCSharp==null)Generator.GetGenConfig(XLua.Utils.GetAllTypes());
            var config = CollectMethods();
            var sourceEdits = new List<AutoTagWrite>();
            var targetTypes = new HashSet<Type>();
            var sourcePaths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach(var target in options.AutoTagTargets) {
                if(target==null)throw new ArgumentException("Null auto-tag target");
                if(!target.Enabled)continue;
                if(UnityEditor.EditorApplication.isCompiling || UnityEditor.EditorUtility.scriptCompilationFailed)
                    throw new InvalidOperationException("Wait for successful script compilation before rebuilding FastCall tags");
                if(target.TargetType==null || !targetTypes.Add(target.TargetType))throw new ArgumentException("Missing or duplicate auto-tag target type");
                var targetMethods = new List<MethodInfo>();
                foreach(var sourcePath in new[]{target.SourcePath}.Concat(target.AdditionalSourcePaths)) {
                    if(string.IsNullOrWhiteSpace(sourcePath))throw new ArgumentException("Auto-tag SourcePath is required");
                    string path=Path.GetFullPath(sourcePath);
                    if(!sourcePaths.Add(path))throw new ArgumentException("Each auto-tag source file may be configured only once");
                    byte[] original=File.ReadAllBytes(path);
                    bool hasBom=original.Length>=3 && original[0]==239 && original[1]==187 && original[2]==191;
                    string source=new UTF8Encoding(false,true).GetString(original,hasBom?3:0,original.Length-(hasBom?3:0));
                    MethodInfo[] selected;
                    string rebuilt=XLuaFastCallAutoTags.RebuildSource(target.TargetType,source,target.ConditionSymbol,out selected);
                    sourceEdits.Add(new AutoTagWrite { Path=path, Original=original, Source=source, Rebuilt=rebuilt });
                    targetMethods.AddRange(selected);
                }
                if(targetMethods.Distinct().Count()!=targetMethods.Count)
                    throw new ArgumentException("Auto-tag method matched more than one configured source file: "+target.TargetType);
                // Current reflection attributes can be stale until domain reload. Use
                // the exact methods selected during this run for callbacks and Lua alike.
                if(targetMethods.Count==0)config.Remove(target.TargetType);
                else config[target.TargetType]=targetMethods.ToArray();
            }
            var files=new Dictionary<string,string>();var names=new HashSet<string>();
            var luaFiles=new Dictionary<string,string>();
            var install=new StringBuilder(Header+ClassHeader+"        partial void InstallConfigured(ref int installed)\n        {\n");
            int typeIndex=0;
            foreach(var entry in config.OrderBy(k=>k.Key.FullName,StringComparer.Ordinal)) {
                var type=entry.Key;
                if(Generator.LuaCallCSharp==null || !Generator.LuaCallCSharp.Contains(type))
                    throw new InvalidOperationException("XLuaFastCall type must also be in XLua LuaCallCSharp configuration: "+type);
                if(!type.IsClass || !type.IsVisible || type.ContainsGenericParameters || !names.Add(type.Name) || type.Name.StartsWith("_") || type.Name=="Enums" || type.Name=="InstallProxyMethods")throw new InvalidOperationException("Invalid or duplicate XLuaFastCall type name: "+type);
                var duplicate = entry.Value.GroupBy(m => m.Name).FirstOrDefault(g => g.Count() > 1);
                if (duplicate != null)
                    throw new InvalidOperationException("XLuaFastCall: mark only one overload per method name: " + type.FullName + "." + duplicate.Key);
                var methods = entry.Value.OrderBy(m => m.Name, StringComparer.Ordinal).ToList();
                foreach (var method in methods) {
                    ValidateMethod(method);
                }
                if(options.LuaOutputDirectory!=null)luaFiles.Add(type.Name+"XLuaFastCall.lua",EmitLua(type,methods));
                var b=new StringBuilder(Header+ClassHeader);
                var ids=new List<string>();
                for(int i=0;i<methods.Count;i++) {
                    string id="Configured_"+typeIndex+"_"+i;ids.Add(id);b.Append(Emit(type,methods[i],id));
                }
                // Register must use these exact delegate instances, including on Mono.
                b.AppendLine("        static readonly Callback[] ConfiguredRoots"+typeIndex+" =");
                b.AppendLine("        {");
                foreach(var id in ids)b.AppendLine("            "+id+",");
                b.AppendLine("        };\n    }\n}");
                files.Add(type.Name+"XLuaFastCall.cs",b.ToString());
                install.AppendLine("            BeginConfiguredType(typeof("+T(type)+"), "+Q(type.Name)+", new string[]");
                install.AppendLine("            {");
                foreach(var m in methods)install.AppendLine("                "+Q(m.Name)+",");
                install.AppendLine("            });\n            try\n            {");
                for(int i=0;i<methods.Count;i++) {
                    var m=methods[i];var inputs=m.GetParameters().Where(p=>!p.IsOut).ToArray();
                    int required=inputs.Count(p=>!p.IsOptional);
                    string strict=required==inputs.Length?"":", \"handle\", "+Q(type.FullName.Replace('+','.')+"."+m.Name);
                    install.AppendLine("                Register("+Q(m.Name)+", ConfiguredRoots"+typeIndex+"["+i+"], new int[] { "+string.Join(", ",inputs.Select(p=>Kind(ValueType(p)).ToString()))+" }, "+required+strict+");");
                    install.AppendLine("                installed++;");
                }
                install.AppendLine("            }\n            finally\n            {\n                EndConfiguredType();\n            }\n");
                typeIndex++;
            }
            install.AppendLine("        }\n    }\n}");
            files.Add("XLuaFastCallRegister.Generated.cs",install.ToString());
            string luaOutput=null;
            if(options.LuaOutputDirectory!=null) {
                if(string.IsNullOrWhiteSpace(options.LuaOutputDirectory))throw new ArgumentException("LuaOutputDirectory must be null (disabled) or a directory");
                luaOutput=Path.GetFullPath(options.LuaOutputDirectory);
            }
            // Validate every configured type before modifying any business source.
            foreach(var edit in sourceEdits)
                if(!File.ReadAllBytes(edit.Path).SequenceEqual(edit.Original))throw new IOException("Auto-tag source changed during generation: "+edit.Path);
            string output=Path.Combine(GeneratorConfig.common_path,"XLuaFastCall");Directory.CreateDirectory(output);
            if(luaOutput!=null)Directory.CreateDirectory(luaOutput);
            foreach(var edit in sourceEdits) {
                if(edit.Source==edit.Rebuilt)continue;
                bool bom=edit.Original.Length>=3 && edit.Original[0]==239 && edit.Original[1]==187 && edit.Original[2]==191;
                File.WriteAllText(edit.Path,edit.Rebuilt,new UTF8Encoding(bom));
            }
            foreach(var file in Directory.GetFiles(output,"*.cs"))
                if(!files.ContainsKey(Path.GetFileName(file)) && (File.ReadAllText(file).StartsWith(Header,StringComparison.Ordinal) || File.ReadAllText(file).StartsWith("// Generated by XLuaFastCallEditor.XLuaFastCallGenerator.",StringComparison.Ordinal)))File.Delete(file);
            foreach(var file in files)File.WriteAllText(Path.Combine(output,file.Key),file.Value,new UTF8Encoding(false));
            if(luaOutput!=null) {
                foreach(var file in Directory.GetFiles(luaOutput,"*XLuaFastCall.lua"))
                    if(!luaFiles.ContainsKey(Path.GetFileName(file)) && File.ReadAllText(file).StartsWith(LuaHeader,StringComparison.Ordinal))File.Delete(file);
                foreach(var file in luaFiles)File.WriteAllText(Path.Combine(luaOutput,file.Key),file.Value,new UTF8Encoding(false));
            }
            Debug.Log("XLuaFastCall Generate finished: "+output);
        }
        const string LuaHeader="-- Generated by XLuaFastCallEditor.XLuaFastCallGenerator.\n";
        static string LuaQuote(string value)=>"\""+value.Replace("\\","\\\\").Replace("\"","\\\"").Replace("\r","\\r").Replace("\n","\\n")+"\"";
        public static string EmitLua(Type type,IEnumerable<MethodInfo> methods) {
            if(type==null || methods==null)throw new ArgumentNullException();
            var b=new StringBuilder(LuaHeader);
            b.AppendLine("local fast = XLuaFastCall["+LuaQuote(type.Name)+"]");
            b.AppendLine("return {");
            foreach(var method in methods.OrderBy(m=>m.Name,StringComparer.Ordinal))
                b.AppendLine("    ["+LuaQuote(method.Name)+"] = fast["+LuaQuote(method.Name)+"],");
            b.AppendLine("}");return b.ToString();
        }
        sealed class AutoTagWrite {
            public string Path, Source, Rebuilt;
            public byte[] Original;
        }
    }
}
