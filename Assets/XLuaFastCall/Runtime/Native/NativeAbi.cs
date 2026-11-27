using System.Runtime.InteropServices;

namespace XLuaFastCall
{
    internal static unsafe class NativeAbi
    {
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        private static extern int sd_layout([Out] int[] values);

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

        // 与 lua_fast_call_bridge.c 的 L_* 布局索引一一对应。
        private enum LayoutIndex
        {
            Slot = 0,
            SlotTag = 1,
            Cell = 2,
            SlotLength = 3,
            FrameCells = 4,
            FrameCount = 5,
            FrameError = 6,
            CellKind = 7,
            CellSize = 8,
            ReservedToken = 9,
            Frame = 10,
            FrameReceiver = 11,
            Count = 12
        }

        internal const int BufferAbiVersion = 1009;
        internal const int TagFloat = 3;
        internal const int TagInteger = 19;
        internal const int Inputs = 32;
        internal const int Outputs = 16;
        internal const int ErrorBytes = 4096;

        // 固定 12 项容量兼容旧查询入口；先返回版本，再校验当前布局。
        internal static bool Matches()
        {
            var layout = new int[(int)LayoutIndex.Count];

            int Off(string field) => (int)Marshal.OffsetOf(typeof(Frame), field);

            return sd_layout(layout) == BufferAbiVersion
                && layout[(int)LayoutIndex.Slot] == sizeof(Slot)
                && layout[(int)LayoutIndex.SlotTag] == 8
                && layout[(int)LayoutIndex.SlotLength] == 12
                && layout[(int)LayoutIndex.Cell] == sizeof(Cell)
                && layout[(int)LayoutIndex.CellKind] == 8
                && layout[(int)LayoutIndex.CellSize] == 16
                && layout[(int)LayoutIndex.Frame] == sizeof(Frame)
                && layout[(int)LayoutIndex.FrameReceiver] == Off(nameof(Frame.Receiver))
                && layout[(int)LayoutIndex.FrameCells] == Off(nameof(Frame.Cells))
                && layout[(int)LayoutIndex.FrameError] == Off(nameof(Frame.Error))
                && layout[(int)LayoutIndex.FrameCount] == Off(nameof(Frame.Count))
                && layout[(int)LayoutIndex.ReservedToken] == 0;
        }
    }
}
