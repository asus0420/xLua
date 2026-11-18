using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using CSObjectWrapEditor;
using XLua;

namespace XLuaFastCallEditor
{
    // Editor-only adapter. xLua owns filtering, static/instance grouping and branch order.
    // Fail generation if the private generation contract changes; never guess a grouping.
    public static class XLuaMethodGroups
    {
        public sealed class Branch
        {
            public MethodInfo Method;
            public int Omitted;
            public int Arity => Method.GetParameters().Take(Method.GetParameters().Length - Omitted)
                .Count(p => !(p.IsOut && p.ParameterType.IsByRef));
        }
        public sealed class Group
        {
            public string Name;
            public bool IsStatic;
            public readonly List<Branch> Branches = new List<Branch>();
            public MethodInfo[] Methods => Branches.Select(b => b.Method).Distinct().ToArray();
        }
        static object Property(object value, string name)
        {
            var p = value.GetType().GetProperty(name);
            if (p == null) throw new InvalidOperationException("xLua group adapter: missing " + name);
            return p.GetValue(value, null);
        }
        public static List<Group> Read(Type type)
        {
            var getter = typeof(Generator).GetMethod("getClassInfo", BindingFlags.Static | BindingFlags.NonPublic,
                null, new[] { typeof(Type), typeof(LuaTable) }, null);
            if (getter == null || getter.ReturnType != typeof(void))
                throw new InvalidOperationException("xLua getClassInfo contract changed; update FastCall editor adapter");
            var result = new List<Group>();
            using (var env = new LuaEnv())
            using (var data = env.NewTable())
            {
                getter.Invoke(null, new object[] { type, data });
                var groups = data.Get<object>("methods") as IEnumerable;
                if (groups == null) throw new InvalidOperationException("xLua group adapter: methods missing");
                foreach (var value in groups)
                {
                    var group = new Group { Name = (string)Property(value, "Name"), IsStatic = (bool)Property(value, "IsStatic") };
                    var methods = ((IEnumerable)Property(value, "Overloads")).Cast<MethodBase>().ToArray();
                    var defaults = ((IEnumerable)Property(value, "DefaultValues")).Cast<int>().ToArray();
                    if (methods.Length == 0 || methods.Length != defaults.Length)
                        throw new InvalidOperationException("xLua group adapter: invalid branch lists");
                    for (int i = 0; i < methods.Length; i++)
                    {
                        var method = methods[i] as MethodInfo;
                        if (method == null || defaults[i] < 0 || defaults[i] > method.GetParameters().Length)
                            throw new InvalidOperationException("xLua group adapter: invalid branch metadata");
                        group.Branches.Add(new Branch { Method = method, Omitted = defaults[i] });
                    }
                    result.Add(group);
                }
            }
            return result;
        }
    }
}
