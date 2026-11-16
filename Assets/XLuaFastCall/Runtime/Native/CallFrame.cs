using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using static XLuaFastCall.NativeAbi;

namespace XLuaFastCall
{
    // 仅在本次原生回调内使用；ref struct 禁止装箱、闭包捕获和存入堆对象。
    // 不得跨越原生回调保存其地址；引用类型限制不能替代原生内存生命周期约束。
    internal unsafe ref partial struct CallFrame
    {
        readonly Slot* args;
        readonly Frame* frame;
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public CallFrame(IntPtr args, IntPtr frame)
        {
            this.args = (Slot*)args;
            this.frame = (Frame*)frame;
        }

        public long Receiver
        {
            [MethodImpl(MethodImplOptions.AggressiveInlining)]
            get => frame->Receiver;
        }

        public int ArgCount
        {
            [MethodImpl(MethodImplOptions.AggressiveInlining)]
            get => frame->Count - 1;
        }

        // ---- 参数读取（语义与原 Binding.Integer/Boolean/Float/StrictNumber/ReadString 相同）
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public long Integer(int i)
        {
            CheckInput(i);
            Slot* s = args + i;
            if (s->Tag == TagInteger)
                return s->Integer;
            if (s->Tag == TagFloat)
            {
                double n = s->Number;
                return n >= long.MinValue && n < 9223372036854775808.0 && Math.Floor(n) == n ? (long)n : 0;
            }

            return frame->Cells[i].Integer; // C 端兼容转换（数字字符串等）写在 cells[i]
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public bool Boolean(int i)
        {
            CheckInput(i);
            Slot* s = args + i;
            return s->Integer != 0; // C normalizes every Boolean slot to 0/1.
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public float Float(int i)
        {
            CheckInput(i);
            Slot* s = args + i;
            if (s->Tag == TagInteger)
                return s->Integer;
            if (s->Tag == TagFloat)
                return (float)s->Number;
            throw new InvalidCastException("Expected Lua number");
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public double StrictNumber(int i)
        {
            CheckInput(i);
            Slot* s = args + i;
            if (s->Tag == TagInteger)
                return s->Integer;
            if (s->Tag == TagFloat)
                return s->Number;
            throw new InvalidCastException("Expected Lua number");
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public string String(int i)
        {
            CheckInput(i);
            Slot* s = args + i;
            return LuaStringCodec.Read((byte*)s->Address, s->Length);
        }

        // ---- 结果写入：i 为第 i 个结果（0..Outputs-1），写到 cells[Inputs+i]。Cell.Kind 存 kind：1 int,2 number,3 bool,6 string
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public void Return(int i, int v)
        {
            Cell* p = Out(i);
            p->Integer = v;
            p->Kind = 1;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public void Return(int i, byte v)
        {
            Cell* p = Out(i);
            p->Integer = v;
            p->Kind = 1;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public void Return(int i, long v)
        {
            Cell* p = Out(i);
            p->Integer = v;
            p->Kind = 1;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public void Return(int i, float v)
        {
            Cell* p = Out(i);
            p->Number = v;
            p->Kind = 2;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public void Return(int i, double v)
        {
            Cell* p = Out(i);
            p->Number = v;
            p->Kind = 2;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public void Return(int i, bool v)
        {
            Cell* p = Out(i);
            p->Integer = v ? 1 : 0;
            p->Kind = 3;
        }

        // 编码进本深度的共享缓冲区（多个字符串结果通过 used 依次排列）。
        // false：UTF8 上界超出缓冲区，调用方须改由 C# 推送全部结果（返回 100+n）。
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public bool ReturnString(int i, string v, ref int used)
        {
            Cell* p = Out(i);
            p->Kind = 6;
            if (!LuaStringCodec.Encode(v, frame->Error, ref used, out byte* output, out long size))
                return false;
            p->Address = (ulong)output;
            p->Size = size;
            return true;
        }

        // No formatting exception may leave the reverse P/Invoke callback.
        [MethodImpl(MethodImplOptions.NoInlining)]
        public int Error(Exception e)
        {
            try
            {
                if (e is LuaStringCodec.PushException push && push.Status > 0)
                    return -1 - push.Status;
                byte[] bytes = Encoding.UTF8.GetBytes(e.ToString());
                int n = Math.Min(ErrorBytes - 1, bytes.Length);
                Marshal.Copy(bytes, 0, (IntPtr)frame->Error, n);
                frame->Error[n] = 0;
            }
            catch
            {
                const string fallback = "XLuaFastCall: exception while formatting C# exception";
                for (int i = 0; i < fallback.Length; i++) frame->Error[i] = (byte)fallback[i];
                frame->Error[fallback.Length] = 0;
            }
            return -1;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        Cell* Out(int i)
        {
#if UNITY_EDITOR || DEVELOPMENT_BUILD
   if((uint)i>=Outputs)throw new IndexOutOfRangeException("XLuaFastCall output "+i);
#endif
            return frame->Cells + Inputs + i;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        static void CheckInput(int i)
        {
#if UNITY_EDITOR || DEVELOPMENT_BUILD
   if((uint)i>=Inputs)throw new IndexOutOfRangeException("XLuaFastCall input "+i);
#endif
        }
    }
}
