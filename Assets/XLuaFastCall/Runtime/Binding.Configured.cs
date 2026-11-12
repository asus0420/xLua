using System;
using XLua;
using LuaAPI = XLua.LuaDLL.Lua;

namespace XLuaFastCall {
    public sealed unsafe partial class Binding {
        static void PushConfigured(IntPtr L,string value) => LuaStringCodec.Push(L,value);
        static void PushConfigured(IntPtr L,int value) => LuaAPI.xlua_pushinteger(L,value);
        static void PushConfigured(IntPtr L,long value) => LuaAPI.lua_pushint64(L,value);
        static void PushConfigured(IntPtr L,float value) => LuaAPI.lua_pushnumber(L,value);
        static void PushConfigured(IntPtr L,double value) => LuaAPI.lua_pushnumber(L,value);
        static void PushConfigured(IntPtr L,bool value) => LuaAPI.lua_pushboolean(L,value);
        // 仅在初始化时执行；运行时回调继续使用既有原生 ABI。
        void BeginConfiguredType(Type type, string name, string[] methods) {
            int typeId = translator.GetTypeId(env.L, type); // 先确保原 XLua Wrap 已注册，之后才能取得其方法表。
            using (var root = env.Global.Get<LuaTable>("XLuaFastCall"))
            using (var fast = env.NewTable()) {
                root.Set(name, fast);
                using (var register = root.Get<LuaFunction>("_RegisterType")) {
                    LuaAPI.xlua_rawgeti(env.L, LuaIndexes.LUA_REGISTRYINDEX, typeId);
                    var meta = translator.GetObject(env.L, -1, typeof(LuaTable)) as LuaTable;
                    LuaAPI.lua_pop(env.L, 1);
                    using (meta) register.Call(meta, name, fast, string.Join("\n", methods));
                }
                translator.Push(env.L, fast); // 将优化方法表压栈，后续 sd_install 把原生入口写入此表。
            }
        }
        void EndConfiguredType() { LuaAPI.lua_pop(env.L, 1); }
    }
}
