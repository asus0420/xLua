using System;
using System.Text;
using System.Reflection;
using Native = XLuaFastCall.LuaTableWriterNative;
using LuaAPI = XLua.LuaDLL.Lua;

namespace XLua
{
    /// <summary>
    /// A temporary stack table. Use only inside the current main-state Lua-to-C# callback.
    /// Globally single threaded. Never cache, cross an await/frame, or use after LuaEnv disposal.
    /// Identity checks are diagnostic aids, not a lifetime guarantee. No FastCall integration.
    /// </summary>
    public readonly struct LuaTableWriter
    {
        readonly IntPtr owner;
        readonly int index;
        readonly IntPtr identity;
        static byte[] utf8 = new byte[256];

        LuaTableWriter(IntPtr owner, int index, IntPtr identity)
        {
            this.owner = owner;
            this.index = index;
            this.identity = identity;
        }

        public static LuaTableWriter New(LuaEnv env, int arrayCapacity = 0, int hashCapacity = 0)
        {
            if (env == null) throw new ArgumentNullException(nameof(env));
            return Create(env.L, 0, IntPtr.Zero, arrayCapacity, hashCapacity);
        }

        public LuaTableWriter NewChild(int arrayCapacity = 0, int hashCapacity = 0)
        {
            if (owner == IntPtr.Zero) throw new InvalidOperationException("Default Writer has no table.");
            return Create(owner, index, identity, arrayCapacity, hashCapacity);
        }

        static LuaTableWriter Create(IntPtr L, int parent, IntPtr ptr, int array, int hash)
        {
            int status = Native.xlua_tw_new(L, parent, ptr, array, hash, out int idx, out IntPtr id);
            // Creation-only error: keep the hot Check used by every Set unchanged.
            if (status == -5)
                throw new InvalidOperationException("LuaTableWriter.New requires an active Lua-to-C# callback on the Lua main state.");
            Check(L, status);
            return new LuaTableWriter(L, idx, id);
        }

        public static void RequireMainThread(IntPtr L)
        {
            if (Native.xlua_tw_main(L) == 0)
                throw new InvalidOperationException(
                    "LuaTableWriter requires the Lua main state; coroutine calls are rejected before business execution.");
        }

        public static void Push(IntPtr L, LuaTableWriter value)
        {
            Check(L, Native.xlua_tw_push(L, value.owner, value.index, value.identity));
        }

        public static void Push(IntPtr L, LuaTableWriter? value)
        {
            if (value.HasValue) Push(L, value.Value);
            else LuaAPI.lua_pushnil(L);
        }

        // Only binding code with an explicitly declared Writer output may call this.
        // Nullable<T> reflection results box as either T or null.
        internal static void PushDeclaredResult(IntPtr L, object value)
        {
            if (value == null) LuaAPI.lua_pushnil(L);
            else Push(L, (LuaTableWriter)value);
        }

        static void Check(IntPtr L, int status)
        {
            if (status == 0) return;
            if (status == 1)
                ObjectTranslatorPool.Instance.Find(L).luaEnv.ThrowExceptionFromError(LuaAPI.lua_gettop(L) - 1);
            throw new InvalidOperationException("LuaTableWriter native operation failed: " + status +
                                                " (-1 stale handle, -2 stack capacity, -3 state mismatch, -4 invalid arguments).");
        }

        static byte[] Encode(string key, string value, out int keyLen, out int valueLen)
        {
            keyLen = key == null ? -1 : Encoding.UTF8.GetByteCount(key);
            valueLen = value == null ? 0 : Encoding.UTF8.GetByteCount(value);
            int prefix = Math.Max(0, keyLen);
            int total = checked(prefix + valueLen);
            if (utf8.Length < total) utf8 = new byte[Math.Max(total, checked(utf8.Length * 2))];
            if (key != null) Encoding.UTF8.GetBytes(key, 0, key.Length, utf8, 0);
            if (value != null) Encoding.UTF8.GetBytes(value, 0, value.Length, utf8, prefix);
            return utf8;
        }

        void Write(int keyKind, long key, string textKey, int kind, long integer,
            double number, string textValue, LuaTableWriter child)
        {
            byte[] bytes = null;
            int keyLen = 0, valueLen = 0;
            if (keyKind == 1 || kind == 4) bytes = Encode(textKey, textValue, out keyLen, out valueLen);
            Check(owner, Native.xlua_tw_set(owner, index, identity, keyKind, key,
                bytes, bytes == null ? 0 : bytes.Length, keyLen, kind, integer, number,
                valueLen, child.owner, child.index, child.identity));
        }

        // Reuse xLua's enum pusher/cache; enum values retain userdata semantics.
        public void Set<TEnum>(long key, TEnum value) where TEnum : struct, Enum =>
            WriteEnum(0, key, null, value);

        public void Set<TEnum>(string key, TEnum value) where TEnum : struct, Enum =>
            WriteEnum(1, 0, key, value);

        void WriteEnum<TEnum>(int keyKind, long key, string textKey, TEnum value)
            where TEnum : struct, Enum
        {
            if (owner == IntPtr.Zero) throw new InvalidOperationException("Default Writer has no table.");
            int oldTop = LuaAPI.lua_gettop(owner);
            try
            {
                // This follows xLua's existing protection boundary. Only Write's native
                // operation is protected by Writer's pcall, not PushByType itself.
                var translator = ObjectTranslatorPool.Instance.Find(owner);
                translator.PushByType(owner, value);
                // Encode the key AFTER pushing, since pushing may run reentrant finalizers.
                Write(keyKind, key, textKey, 6, 0, 0, null, default);
            }
            finally
            {
                // Native kind 6 borrows the value: it never consumes the original slot.
                LuaAPI.lua_settop(owner, oldTop);
            }
        }

        public void Set(long key, int value) => Write(0, key, null, 1, value, 0, null, default);
        public void Set(long key, long value) => Write(0, key, null, 1, value, 0, null, default);
        public void Set(long key, uint value) => Write(0, key, null, 1, value, 0, null, default);
        public void Set(long key, float value) => Write(0, key, null, 2, 0, value, null, default);
        public void Set(long key, double value) => Write(0, key, null, 2, 0, value, null, default);
        public void Set(long key, bool value) => Write(0, key, null, 3, value ? 1 : 0, 0, null, default);
        public void Set(long key, string value) => Write(0, key, null, value == null ? 0 : 4, 0, 0, value, default);
        public void Set(long key, LuaTableWriter value) => Write(0, key, null, 5, 0, 0, null, value);

        public void Set(long key, LuaTableWriter? value) => Write(0, key, null, value.HasValue ? 5 : 0, 0, 0, null,
            value.GetValueOrDefault());

        public void Set(string key, int value) => Write(1, 0, key, 1, value, 0, null, default);
        public void Set(string key, long value) => Write(1, 0, key, 1, value, 0, null, default);
        public void Set(string key, uint value) => Write(1, 0, key, 1, value, 0, null, default);
        public void Set(string key, float value) => Write(1, 0, key, 2, 0, value, null, default);
        public void Set(string key, double value) => Write(1, 0, key, 2, 0, value, null, default);
        public void Set(string key, bool value) => Write(1, 0, key, 3, value ? 1 : 0, 0, null, default);
        public void Set(string key, string value) => Write(1, 0, key, value == null ? 0 : 4, 0, 0, value, default);
        public void Set(string key, LuaTableWriter value) => Write(1, 0, key, 5, 0, 0, null, value);

        public void Set(string key, LuaTableWriter? value) =>
            Write(1, 0, key, value.HasValue ? 5 : 0, 0, 0, null, value.GetValueOrDefault());

        internal static bool IsWriter(Type type)
        {
            if (type.IsByRef) type = type.GetElementType();
            return type == typeof(LuaTableWriter) || Nullable.GetUnderlyingType(type) == typeof(LuaTableWriter);
        }

        // Called once per reflected binding. Only ordinary method return values support Writer.
        internal static bool ValidateSignature(MethodBase method)
        {
            bool output = method is MethodInfo info && IsWriter(info.ReturnType);
            foreach (var parameter in method.GetParameters())
            {
                var type = parameter.ParameterType;
                var element = type.HasElementType ? type.GetElementType() : type;
                if (!IsWriter(type) && !IsWriter(element)) continue;
                throw new NotSupportedException("LuaTableWriter only supports method return values: " + method);
            }

            if (output && (method.IsSpecialName || ((MethodInfo)method).ReturnType.IsByRef))
                throw new NotSupportedException("LuaTableWriter only supports ordinary method return values: " + method);
            return output;
        }
    }
}
