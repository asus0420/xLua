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

        [DllImport(NativeAbi.Library, CallingConvention = CallingConvention.Cdecl)]
        static extern int sd_install_proxy(IntPtr L, string name, IntPtr cb, int[] kinds, int count, int required, int allowExtra, string handleField, string strictName);

        [DllImport(NativeAbi.Library, CallingConvention = CallingConvention.Cdecl)]
        static extern int sd_install_group(IntPtr L, string name, IntPtr[] callbacks, int[] counts, int[] arities, int[] kinds, int branches, string handleField, string strictName);

        readonly LuaEnv env;
        readonly ObjectTranslator translator;
        readonly int thread = System.Threading.Thread.CurrentThread.ManagedThreadId;
        bool closed;

        // 注册逻辑仅在初始化执行，回调保留原生快速路径。
        partial void InstallConfigured(ref int installed);

        partial void ConfiguredGenerationPresent(ref bool present);

        // 借用 Lua 调用方传入的包装对象，仅在构造期间持有。
        LuaTable configuredRoot;
        LuaFunction configuredRegisterType;

        public Binding(LuaEnv env, LuaTable root, LuaFunction registerType)
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
                bool generated = false;
                ConfiguredGenerationPresent(ref generated);

                if (!generated)
                    throw new InvalidOperationException("Run XLua/Generate Code before creating a configured binding");

                configuredRoot = root;
                configuredRegisterType = registerType;

                try
                {
                    InstallConfigured(ref installed);
                }
                finally
                {
                    configuredRoot = null;
                    configuredRegisterType = null;
                }
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

        // 生成的代理入口不接受整数注册表句柄作为 self。
        void Register(string name, Callback cb, int[] kinds, int required, bool allowExtra,
            string handleField = "handle", string strictName = null, bool userdataOnly = true)
        {
            if (!userdataOnly)
                throw new ArgumentException("Proxy registrations require userdata self", nameof(userdataOnly));

            if (sd_install_proxy(env.L, name, Marshal.GetFunctionPointerForDelegate(cb), kinds, kinds.Length,
                required, allowExtra ? 1 : 0, handleField, strictName) != 0)
                throw new InvalidOperationException("Registration failed: " + name);
        }

        void RegisterGroup(string name, Callback[] callbacks, int[][] kinds, int[] arities, string strictName)
        {
            if (callbacks.Length != kinds.Length || kinds.Length != arities.Length)
                throw new ArgumentException("Group descriptor count mismatch");

            var pointers = new IntPtr[callbacks.Length];
            var counts = new int[callbacks.Length];
            var flat = new int[callbacks.Length * MaxInputs];

            for (int i = 0; i < callbacks.Length; i++)
            {
                if (kinds[i].Length > MaxInputs)
                    throw new ArgumentException("Too many group inputs");

                pointers[i] = Marshal.GetFunctionPointerForDelegate(callbacks[i]);
                counts[i] = kinds[i].Length;
                Array.Copy(kinds[i], 0, flat, i * MaxInputs, counts[i]);
            }

            if (sd_install_group(env.L, name, pointers, counts, arities, flat, callbacks.Length, "handle", strictName) != 0)
                throw new InvalidOperationException("Group registration failed: " + name);
        }

        // 仅在对象池开始新一轮使用时清除缓存；常规调用不进入此方法。
        [DllImport(NativeAbi.Library, CallingConvention = CallingConvention.Cdecl)]
        static extern int sd_reset_receiver(IntPtr L, int index);

        public void ResetReceiver(object receiver)
        {
            Check();

            if (receiver == null)
                throw new ArgumentNullException(nameof(receiver));

            int top = XLua.LuaDLL.Lua.lua_gettop(env.L);

            try
            {
                translator.Push(env.L, receiver);

                if (sd_reset_receiver(env.L, -1) != 0)
                    throw new InvalidOperationException("XLuaFastCall Proxy uservalue is already in use or receiver is not userdata");
            }
            finally
            {
                XLua.LuaDLL.Lua.lua_settop(env.L, top);
            }
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
