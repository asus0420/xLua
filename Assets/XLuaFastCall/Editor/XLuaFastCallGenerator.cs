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
        const string Header = "// 由 XLuaFastCallEditor.XLuaFastCallGenerator 自动生成，请勿手动修改。\n";
        const string ClassHeader = "using System;\n\nnamespace XLuaFastCall\n{\n    public sealed unsafe partial class Binding\n    {\n";
        static string Q(string s) => "\""+s.Replace("\\","\\\\").Replace("\"","\\\"").Replace("\r","\\r").Replace("\n","\\n")+"\"";
        static string T(Type t) => "global::"+t.FullName.Replace('+','.');
        static Type ValueType(ParameterInfo p) => p.ParameterType.IsByRef ? p.ParameterType.GetElementType() : p.ParameterType;
        static bool NumberEnum(Type t) => t.IsEnum && new[]{typeof(int),typeof(byte),typeof(short),typeof(sbyte),typeof(ushort)}.Contains(Enum.GetUnderlyingType(t));
        static bool Supported(Type t) => new[]{typeof(int),typeof(long),typeof(float),typeof(double),typeof(bool),typeof(string)}.Contains(t) || NumberEnum(t);
        // 仅在 Editor 生成阶段扫描配置，不在运行时执行反射。
        static IEnumerable<object> ConfigValues<TAttribute>() where TAttribute:Attribute {
            foreach(var type in XLua.Utils.GetAllTypes().Where(t=>t.IsAbstract && t.IsSealed)) {
                foreach(var field in type.GetFields(BindingFlags.Static|BindingFlags.Public|BindingFlags.NonPublic))
                    if(field.IsDefined(typeof(TAttribute),false))yield return field.GetValue(null);
                foreach(var prop in type.GetProperties(BindingFlags.Static|BindingFlags.Public|BindingFlags.NonPublic))
                    if(prop.IsDefined(typeof(TAttribute),false))yield return prop.GetValue(null,null);
            }
        }
        // 校验签名是否能由当前标量传参协议表达；不支持时保留原 Wrap。
        public static string UnsupportedReason(MethodInfo m) {
            if(m.IsGenericMethod || m.IsSpecialName || m.IsStatic)return "generic/static/special method";
            if(m.IsDefined(typeof(XLua.BlackListAttribute),true))return "XLua BlackList";
            var ps=m.GetParameters();
            if(ps.Any(p=>p.IsOptional || p.IsDefined(typeof(ParamArrayAttribute),false) || (p.IsIn && p.ParameterType.IsByRef)))return "optional/params/in parameters";
            if((m.ReturnType!=typeof(void)&&!Supported(m.ReturnType)) || ps.Any(p=>!Supported(ValueType(p))))return "unsupported type (use scalar numbers, bool, string, Int32-channel enums)";
            if(ps.Count(p=>!p.IsOut)>8)return "more than 8 input slots";
            if((m.ReturnType==typeof(void)?0:1)+ps.Count(p=>p.ParameterType.IsByRef)>4)return "more than 4 output slots";
            return null;
        }
        static int Kind(Type t) => t==typeof(string)?7:t==typeof(bool)?3:(t==typeof(float)||t==typeof(double))?10:1;
        static string Read(Type t,int i) {
            if(t==typeof(string))return "ReadString(args, "+i+")";
            if(t==typeof(bool))return "Boolean(args, "+i+")";
            if(t==typeof(float))return "Float(args, "+i+")";
            if(t==typeof(double))return "StrictNumber(args, "+i+")";
            return "("+T(t)+")"+(t.IsEnum?"(int)":"")+"Integer(args, cells, "+i+")";
        }
        static string Scalar(Type t,string n) => t.IsEnum ? "(int)"+n : n;
        // 生成直接查句柄、调用目标方法并写入结果单元的专属回调。
        static string Emit(Type type,MethodInfo m,string id) {
            var b=new StringBuilder();
            b.AppendLine("        [AOT.MonoPInvokeCallback(typeof(Callback))]");
            b.AppendLine("        static int "+id+"(IntPtr L, Slot* args, Frame* frame)");
            b.AppendLine("        {");
            b.AppendLine("            try");
            b.AppendLine("            {");
            b.AppendLine("                Cell* cells = frame->Cells;");
            b.AppendLine("                var target = global::XLuaFastCall.ProxyRegistry.Get<"+T(type)+">(frame->Receiver);");
            var args=new List<string>();var results=new List<KeyValuePair<Type,string>>();int input=0;
            if(m.ReturnType!=typeof(void))results.Add(new KeyValuePair<Type,string>(m.ReturnType,"result"));
            foreach(var p in m.GetParameters()) {
                var t=ValueType(p);string n="p"+p.Position;
                b.AppendLine("                "+T(t)+" "+n+(p.IsOut?";":" = "+Read(t,input++)+";"));
                args.Add((p.IsOut?"out ":p.ParameterType.IsByRef?"ref ":"")+n);
                if(p.ParameterType.IsByRef)results.Add(new KeyValuePair<Type,string>(t,n));
            }
            b.AppendLine();
            b.AppendLine("                "+(m.ReturnType==typeof(void)?"":"var result = ")+"target.@"+m.Name+"("+string.Join(", ",args)+");");
            b.AppendLine();
            bool strings=results.Any(r=>r.Key==typeof(string));
            bool singleString=strings && results.Count==1;
            if(strings)b.AppendLine("                int used = 0;");
            if(strings && !singleString)b.AppendLine("                bool fits = true;");
            for(int i=0;i<results.Count;i++) {
                var r=results[i];
                if(singleString) {
                    b.AppendLine("                if (!WriteString(cells + 8, "+r.Value+", frame->Error, ref used))");
                    b.AppendLine("                {");
                    b.AppendLine("                    LuaStringCodec.Push(L, "+r.Value+");");
                    b.AppendLine("                    return 101;");
                    b.AppendLine("                }");
                }
                else b.AppendLine("                "+(r.Key==typeof(string)?"fits &= WriteString(cells + "+(8+i)+", "+r.Value+", frame->Error, ref used);":"Write(cells + "+(8+i)+", "+Scalar(r.Key,r.Value)+");"));
            }
            if(strings && !singleString) {
                b.AppendLine("                if (!fits)");
                b.AppendLine("                {");
                foreach(var r in results)b.AppendLine("                    PushConfigured(L, "+Scalar(r.Key,r.Value)+");");
                b.AppendLine("                    return "+(100+results.Count)+";");
                b.AppendLine("                }");
            }
            b.AppendLine("                return "+results.Count+";");
            b.AppendLine("            }");
            b.AppendLine("            catch (Exception e)");
            b.AppendLine("            {");
            b.AppendLine("                return Error(e, frame->Error);");
            b.AppendLine("            }");
            b.AppendLine("        }");
            b.AppendLine();
            return b.ToString();
        }
        // XLua Generate Code 会在常规 Wrap 生成之后调用此扩展入口。
        [GenCodeMenu]
        public static void Generate() {
            var config=new Dictionary<Type,string[]>();
            foreach(var value in ConfigValues<XLuaFastCallAttribute>()) {
                if(!(value is IDictionary<Type,string[]> entries))throw new InvalidOperationException("XLuaFastCall must be IDictionary<Type,string[]>");
                foreach(var pair in entries)config.Add(pair.Key,pair.Value);
            }
            var files=new Dictionary<string,string>();var names=new HashSet<string>();
            int errorCount=0;
            var install=new StringBuilder(Header+ClassHeader+"        partial void InstallConfigured(ref int installed)\n        {\n");
            int typeIndex=0;
            foreach(var entry in config.OrderBy(k=>k.Key.FullName,StringComparer.Ordinal)) {
                var type=entry.Key;
                if(Generator.LuaCallCSharp==null || !Generator.LuaCallCSharp.Contains(type))
                    throw new InvalidOperationException("XLuaFastCall type must also be in XLua LuaCallCSharp configuration: "+type);
                if(!type.IsClass || !type.IsVisible || type.ContainsGenericParameters || !names.Add(type.Name) || type.Name.StartsWith("_") || type.Name=="Enums" || type.Name=="InstallProxyMethods")throw new InvalidOperationException("Invalid or duplicate XLuaFastCall type name: "+type);
                if(entry.Value==null || entry.Value.Distinct().Count()!=entry.Value.Length)throw new InvalidOperationException("Null/duplicate method selection: "+type);
                var methods=new List<MethodInfo>();
                // 同名重载无法仅凭方法名确定入口，直接报错，不猜测调用哪个签名。
                foreach(var name in entry.Value.OrderBy(x=>x,StringComparer.Ordinal)) {
                    var candidates=type.GetMethods(BindingFlags.Public|BindingFlags.Instance).Where(m=>m.Name==name).ToArray();
                    string reason=candidates.Length!=1?"missing method or overloaded name":UnsupportedReason(candidates[0]);
                    if(reason!=null) {
                        Debug.LogError("XLuaFastCall: configured method "+type.FullName+"."+name+" cannot be generated: "+reason+"; original Wrap retained.");
                        errorCount++;
                        continue;
                    }
                    methods.Add(candidates[0]);
                }
                if(methods.Count==0)continue;
                var b=new StringBuilder(Header+ClassHeader);
                var ids=new List<string>();
                for(int i=0;i<methods.Count;i++) {
                    string id="Configured_"+typeIndex+"_"+i;ids.Add(id);b.Append(Emit(type,methods[i],id));
                }
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
                    var m=methods[i];var inputs=m.GetParameters().Where(p=>!p.IsOut).Select(ValueType).ToArray();
                    install.AppendLine("                Register("+Q(m.Name)+", "+ids[i]+", new int[] { "+string.Join(", ",inputs.Select(t=>Kind(t).ToString()))+" }, "+inputs.Length+", false);");
                    install.AppendLine("                installed++;");
                }
                install.AppendLine("                GC.KeepAlive(ConfiguredRoots"+typeIndex+");\n            }\n            finally\n            {\n                EndConfiguredType();\n            }\n");
                typeIndex++;
            }
            install.AppendLine("        }\n    }\n}");
            files.Add("XLuaFastCallRegister.Generated.cs",install.ToString());
            string output=Path.Combine(GeneratorConfig.common_path,"XLuaFastCall");Directory.CreateDirectory(output);
            // 只清理本生成器留下的失效文件，避免删除用户自行编写的代码。
            foreach(var file in Directory.GetFiles(output,"*.cs"))
                if(!files.ContainsKey(Path.GetFileName(file)) && (File.ReadAllText(file).StartsWith(Header,StringComparison.Ordinal) || File.ReadAllText(file).StartsWith("// Generated by "+typeof(XLuaFastCallGenerator).FullName+".",StringComparison.Ordinal)))File.Delete(file);
            foreach(var file in files)File.WriteAllText(Path.Combine(output,file.Key),file.Value,new UTF8Encoding(false));
            Debug.Log("XLuaFastCall Generate finished: "+output+"; unsupported configured methods: "+errorCount);
        }
    }
}
