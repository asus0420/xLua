using System;
using System.Runtime.CompilerServices;

namespace XLuaFastCall
{
    // 进程内共享的对象句柄表，不依赖 XLua；句柄由槽位编号和代数组成。
    public static partial class ProxyRegistry
    {
        const int SlotBits=20,SlotMask=(1<<SlotBits)-1,MaxGeneration=int.MaxValue;
        struct Entry { public object Target; public int Generation,Next; }
        static Entry[] entries=new Entry[16];
        static int count,free=-1,ownerThread;
        static void CheckOwner()
        {
            int thread=System.Threading.Thread.CurrentThread.ManagedThreadId;
            if(ownerThread==0)ownerThread=thread;
            if(ownerThread!=thread)throw new InvalidOperationException("Proxy registry requires its owning Lua thread");
        }
        // 优先复用空闲槽位；返回值的高位是代数，低位是数组下标。
        public static long Register(object target)
        {
            CheckOwner();
            if(target==null)throw new ArgumentNullException(nameof(target));
            int index;
            if(free>=0) { index=free;free=entries[index].Next; }
            else {
                if(count>SlotMask)throw new InvalidOperationException("Proxy slot limit reached");
                if(count==entries.Length)Array.Resize(ref entries,Math.Min(entries.Length*2,SlotMask+1));
                index=count++;entries[index].Generation=1;
            }
            entries[index].Target=target;entries[index].Next=-1;
            return ((long)entries[index].Generation<<SlotBits)|(uint)index;
        }
        // 直接通过数组下标取对象，并校验代数和类型，拒绝过期或类型错误的句柄。
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static T Get<T>(long handle) where T:class
        {
            int index=(int)(handle&SlotMask);
            long generation=handle>>SlotBits;
            if(generation<=0 || generation>MaxGeneration || (uint)index>=(uint)count)
                throw new InvalidOperationException("Invalid Proxy handle");
            ref Entry slot=ref entries[index];
            if(slot.Generation!=generation || slot.Target==null)
                throw new InvalidOperationException("Expired Proxy handle");
            if(!(slot.Target is T typed))throw new InvalidOperationException("Proxy handle belongs to another type");
            return typed;
        }
        // 清除强引用并递增代数，使已释放对象的旧句柄立即失效。
        public static void Release(long handle)
        {
            CheckOwner();Get<object>(handle);
            int index=(int)(handle&SlotMask);
            ref Entry slot=ref entries[index];slot.Target=null;
            // 代数耗尽后永久停用此槽位，避免旧句柄重新变成有效句柄。
            if(slot.Generation==MaxGeneration)return;
            slot.Generation++;slot.Next=free;free=index;
        }
    }
}
