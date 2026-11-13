using System;

namespace XLua
{
    // 标记需要生成 FastCall 的具体实例方法；同名重载只标记一个版本。
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = false, Inherited = false)]
    public sealed class XLuaFastCallAttribute : Attribute { }
}
