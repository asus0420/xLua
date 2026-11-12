using System;

namespace XLuaFastCallEditor
{
    // 标记业务项目中的“类型 → 方法名数组”配置，生成器据此选择优化入口。
    [AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
    public sealed class XLuaFastCallAttribute : Attribute { }
}
