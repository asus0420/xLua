using System;
using System.Runtime.InteropServices;
using XLua;

namespace XLuaFastCall
{
    // 每个 LuaEnv 对应一个优化绑定；原生实现编入 xlua.dll。
    public sealed partial class Binding : IDisposable
    {
        public const int MaxInputs = NativeAbi.Inputs, MaxOutputs = NativeAbi.Outputs;
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate int Callback(IntPtr L, IntPtr args, IntPtr frame);
        [DllImport(NativeAbi.Library, CallingConvention = CallingConvention.Cdecl)]
        static extern int sd_open(IntPtr L);
        [DllImport(NativeAbi.Library, CallingConvention = CallingConvention.Cdecl)]
        static extern int sd_install(IntPtr L, string name, IntPtr cb, int[] kinds, int count, int required, string handleField, string strictName);
        [DllImport(NativeAbi.Library, CallingConvention = CallingConvention.Cdecl)]
        static extern int sd_close(IntPtr L);
        readonly LuaEnv env;
        readonly ObjectTranslator translator;
        readonly int thread = System.Threading.Thread.CurrentThread.ManagedThreadId;
        bool closed;
        // 注册逻辑仅在初始化执行，回调保留原生快速路径。
        partial void InstallConfigured(ref int installed);
        public Binding(LuaEnv env)
        {
            this.env = env;
            translator = ObjectTranslatorPool.Instance.Find(env.L);
            if (!NativeAbi.Matches())
                throw new InvalidOperationException("XLuaFastCall buffer ABI mismatch (rebuild xlua native library)");
            LuaStringCodec.Initialize();
            bool opened = false;
            try
            {
                if (sd_open(env.L) != 0)
                    throw new InvalidOperationException("XLuaFastCall open failed");
                opened = true;
                int installed = 0;
                InstallConfigured(ref installed);
                if (installed == 0)
                    throw new InvalidOperationException("Run XLua/Generate Code before creating a configured binding");
            }
            catch
            {
                if (opened)
                    sd_close(env.L);
                throw;
            }
        }

        void Register(string name, Callback cb, int[] kinds, string handleField = "handle")
        {
            Register(name, cb, kinds, kinds.Length, handleField);
        }

        void Register(string name, Callback cb, int[] kinds, int required, string handleField = "handle", string strictName = null)
        {
            if (sd_install(env.L, name, Marshal.GetFunctionPointerForDelegate(cb), kinds, kinds.Length, required, handleField, strictName) != 0)
                throw new InvalidOperationException("Registration failed: " + name);
        }

        // 仅在对象池开始新一轮使用时清除缓存；常规调用不进入此方法。
        [DllImport(NativeAbi.Library, CallingConvention = CallingConvention.Cdecl)]
        static extern int sd_reset_receiver(IntPtr L, int index);
        public void ResetReceiver(object receiver)
        {
            Check();
            if (receiver == null) throw new ArgumentNullException(nameof(receiver));
            int top = XLua.LuaDLL.Lua.lua_gettop(env.L);
            try
            {
                translator.Push(env.L, receiver);
                if (sd_reset_receiver(env.L, -1) != 0)
                    throw new InvalidOperationException("XLuaFastCall Proxy uservalue is already in use or receiver is not userdata");
            }
            finally { XLua.LuaDLL.Lua.lua_settop(env.L, top); }
        }

        void Check()
        {
            if (closed)
                throw new ObjectDisposedException(nameof(Binding));
            if (thread != System.Threading.Thread.CurrentThread.ManagedThreadId)
                throw new InvalidOperationException("Use owning Lua thread");
        }

        public void Dispose()
        {
            if (closed)
                return;
            Check();
            if (sd_close(env.L) != 0)
                throw new InvalidOperationException("Active callback");
            closed = true;
            LuaStringCodec.ReleaseScratch();
        }
    }
}
