using System;
using System.Runtime.InteropServices;

namespace XLuaFastCall
{
    internal static unsafe class NativeAbi
    {
#if (UNITY_IOS || UNITY_IPHONE || UNITY_TVOS || UNITY_WEBGL || UNITY_SWITCH) && !UNITY_EDITOR
        internal const string Library = "__Internal";
#else
        internal const string Library = "xlua";
#endif
        [StructLayout(LayoutKind.Explicit, Size = 16)]
        internal struct Slot
        {
            [FieldOffset(0)]
            public long Integer;
            [FieldOffset(0)]
            public double Number;
            [FieldOffset(0)]
            public ulong Address;
            [FieldOffset(8)]
            public int Tag;
            [FieldOffset(12)]
            public int Length;
        }

        [StructLayout(LayoutKind.Explicit, Size = 24)]
        internal struct Cell
        {
            [FieldOffset(0)]
            public long Integer;
            [FieldOffset(0)]
            public double Number;
            [FieldOffset(0)]
            public ulong Address;
            [FieldOffset(8)]
            public long Kind;
            [FieldOffset(16)]
            public long Size;
        }

        // 64 位字段放在首位，避免 i386 GCC 与 MSVC/C# 的 int64 对齐差异。
        [StructLayout(LayoutKind.Sequential)]
        internal struct Frame
        {
            public long Receiver;
            public Cell* Cells;
            public byte* Error;
            public int Count;
            // 与 C 端显式补齐一致，统一 32 位 GCC 和 MSVC 的 Frame 大小。
            public int Reserved;
        }

        // 与 bridge.c 的 L_* / S_* 一一对应。
        const int LayoutSlot = 0, LayoutSlotTag = 1, LayoutCell = 2, LayoutSlotLength = 3, LayoutFrameCells = 4,
                  LayoutFrameCount = 5, LayoutFrameError = 6, LayoutCellKind = 7, LayoutCellSize = 8, LayoutReservedToken = 9,
                  LayoutFrame = 10, LayoutFrameReceiver = 11, LayoutCount = 12;
        internal const int BufferAbiVersion = 1009;
        internal const int TagFloat = 3, TagInteger = 19;
        internal const int Inputs = 32, Outputs = 16, ErrorBytes = 4096;
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        static extern int sd_layout([Out] int[] values);
        // 固定 12 项容量兼容旧查询入口；先返回版本，再校验当前布局。
        internal static bool Matches()
        {
            var layout = new int[LayoutCount];
            int Off(string field) => (int)Marshal.OffsetOf(typeof(Frame), field);
            return sd_layout(layout) == BufferAbiVersion
                && layout[LayoutSlot] == sizeof(Slot)
                && layout[LayoutSlotTag] == 8
                && layout[LayoutSlotLength] == 12
                && layout[LayoutCell] == sizeof(Cell)
                && layout[LayoutCellKind] == 8
                && layout[LayoutCellSize] == 16
                && layout[LayoutFrame] == sizeof(Frame)
                && layout[LayoutFrameReceiver] == Off(nameof(Frame.Receiver))
                && layout[LayoutFrameCells] == Off(nameof(Frame.Cells))
                && layout[LayoutFrameError] == Off(nameof(Frame.Error))
                && layout[LayoutFrameCount] == Off(nameof(Frame.Count))
                && layout[LayoutReservedToken] == 0;
        }
    }
}
