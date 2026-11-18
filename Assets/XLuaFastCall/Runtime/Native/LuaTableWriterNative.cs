using System;
using System.Runtime.InteropServices;

namespace XLuaFastCall
{
    internal static class LuaTableWriterNative
    {
        [DllImport(NativeAbi.Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int xlua_tw_main(IntPtr L);
        [DllImport(NativeAbi.Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int xlua_tw_new(IntPtr L, int parent, IntPtr parentPtr,
            int array, int hash, out int index, out IntPtr identity);
        [DllImport(NativeAbi.Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int xlua_tw_set(IntPtr L, int index, IntPtr identity,
            int keyKind, long key, [In] byte[] bytes, int bytesLength, int keyLength,
            int kind, long integer, double number, int valueLength,
            IntPtr childOwner, int childIndex, IntPtr childIdentity);
        [DllImport(NativeAbi.Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int xlua_tw_push(IntPtr L, IntPtr owner, int index, IntPtr identity);
    }
}
