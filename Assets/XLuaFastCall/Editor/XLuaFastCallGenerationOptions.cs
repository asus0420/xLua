using System;
using System.Collections.Generic;

namespace XLuaFastCallEditor
{
    // Project configuration belongs outside the framework. Without a subscriber,
    // Generate only reads existing attributes and writes the original C# outputs.
    public sealed class XLuaFastCallGenerationOptions
    {
        public readonly List<XLuaFastCallAutoTagTarget> AutoTagTargets = new List<XLuaFastCallAutoTagTarget>();
        public string LuaOutputDirectory { get; set; }
    }

    public sealed class XLuaFastCallAutoTagTarget
    {
        public bool Enabled { get; set; }
        public Type TargetType { get; set; }
        public string SourcePath { get; set; }
        // Other partial declarations of the same target type. The generator merges
        // the exact methods from all configured files before emitting callbacks.
        public readonly List<string> AdditionalSourcePaths = new List<string>();
        public string ConditionSymbol { get; set; }
    }
}
