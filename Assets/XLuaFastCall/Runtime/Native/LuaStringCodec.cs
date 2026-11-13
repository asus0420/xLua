using System;
using System.Runtime.InteropServices;
using System.Text;

namespace XLuaFastCall
{
    // 读取 C 端提供的字符串内容，编码 UTF-8 结果；不依赖 Lua 内部布局。
    // 仅在所属 Lua 线程使用，缓存通过内容副本校验，可服务该线程的多个 LuaEnv。
    internal static unsafe partial class LuaStringCodec
    {
        // 短文本按内容地址映射缓存；同时比较私有字节副本，防止 GC 后地址复用返回旧文本。
        public const int CacheEntries=256,CacheEntryBytes=64,CacheMaxLength=40,BufferBytes=4096;
        static int ownerThread;
        static readonly string[] cached=new string[CacheEntries];
        static byte* cache;
        public static void Initialize()
        {
            int thread=System.Threading.Thread.CurrentThread.ManagedThreadId;
            if(cache!=null) {
                if(thread!=ownerThread)throw new InvalidOperationException("LuaStringCodec requires its owning Lua thread");
                return;
            }
            cache=(byte*)Marshal.AllocHGlobal(CacheEntries*CacheEntryBytes);
            for(int i=0;i<CacheEntries*CacheEntryBytes;i+=8)*(long*)(cache+i)=0;
            ownerThread=thread;
        }
        // 仅访问 C 端提供的有效内容区，不访问 TString、哈希或内部长度字段。
        public static string Read(byte* data,int len)
        {
            if(data==null)return null;
            if(len>CacheMaxLength)return Decode(data,len);
            ulong address=(ulong)data;
            int slot=(int)(((address>>3)^(address>>11))&(CacheEntries-1));
            byte* entry=cache+slot*CacheEntryBytes;
            if(*(ulong*)entry==address && *(int*)(entry+8)==len && Same(entry+16,data,len))return cached[slot];
            string text=Decode(data,len);
            *(ulong*)entry=address;*(int*)(entry+8)=len;
            for(int k=0;k<len;k++)entry[16+k]=data[k];
            cached[slot]=text;
            return text;
        }
        static bool Same(byte* a,byte* b,int len)
        {
            int k=0;
            for(;k+8<=len;k+=8)if(*(ulong*)(a+k)!=*(ulong*)(b+k))return false;
            for(;k<len;k++)if(a[k]!=b[k])return false;
            return true;
        }
        // ASCII 快速路径：每次检查 8 字节；发现高位非零则交给 UTF8 解码。
        public static string Decode(byte* data,int len)
        {
            if(len==0)return string.Empty;
            int k=0;
            for(;k+8<=len;k+=8)if((*(ulong*)(data+k)&0x8080808080808080UL)!=0)return Encoding.UTF8.GetString(data,len);
            for(;k<len;k++)if(data[k]>=0x80)return Encoding.UTF8.GetString(data,len);
            string s=new string('\0',len);
            fixed(char* p=s)for(k=0;k<len;k++)p[k]=(char)data[k];
            return s;
        }
        // 从 buffer+used 开始编码并推进 used，让多个字符串返回值共享同一个缓冲区。
        // null 输出空指针；空字符串输出非空指针且长度为 0。
        // 若 UTF8 长度上界（ASCII 前缀加剩余字符每个最多 3 字节）超过剩余空间，则返回 false。
        public static bool Encode(string v,byte* buffer,ref int used,out byte* output,out long size)
        {
            output=null;size=0;
            if(v==null)return true;
            int len=v.Length;
            byte* o=buffer+used;
            if(len==0) { output=o;return true; }
            int room=BufferBytes-used;
            if(len>room)return false;
            fixed(char* s=v) {
                int n=NarrowAscii(s,o,len);
                if(n<len) {
                    if(n+(long)(len-n)*3>room)return false;
                    n+=Encoding.UTF8.GetBytes(s+n,len-n,o+n,room-n);
                }
                output=o;size=n;used+=n;return true;
            }
        }
        // 边检查边复制 ASCII 前缀，每次处理 4 个字符，返回已复制的字符数。
        // 剩余部分从非 ASCII 字符开始，因此不会拆开 UTF16 代理项对。
        static int NarrowAscii(char* s,byte* o,int len)
        {
            int k=0;
            for(;k+4<=len;k+=4) {
                ulong c=*(ulong*)(s+k);
                if((c&0xFF80FF80FF80FF80UL)!=0)break;
                *(uint*)(o+k)=(uint)(c&0xFF)|(uint)((c>>8)&0xFF00)|(uint)((c>>16)&0xFF0000)|(uint)((c>>24)&0xFF000000);
            }
            for(;k<len && s[k]<0x80;k++)o[k]=(byte)s[k];
            return k;
        }

        const string LUADLL=NativeAbi.Library;
        [DllImport(LUADLL,CallingConvention=CallingConvention.Cdecl)] static extern void lua_pushnil(IntPtr L);
        [DllImport(LUADLL,CallingConvention=CallingConvention.Cdecl)] static extern int sd_pushlstring(IntPtr L,byte* str,int size);
        internal sealed class PushException : Exception
        {
            internal readonly int Status;
            internal PushException(int status) { Status=status; }
        }
        static void PushBytes(IntPtr L,byte* data,int size)
        {
            int status=sd_pushlstring(L,data,size);
            if(status!=0)throw new PushException(status);
        }
        // 超出当前调用深度缓冲区的返回值使用共享临时缓冲，从 256 字节按倍数扩容，最多保留 64KB。
        // 更大的字符串使用独立临时块，入栈后释放；lua_pushlstring 会先复制内容，再执行 GC。
        // 因此 GC 期间的嵌套调用可以复用共享缓冲，不会覆盖尚未复制的字符串内容。
        // 与短字符串缓存一样，只允许所属 Lua 线程访问。
        const int ScratchKeepBytes=64*1024;
        static byte* scratch;
        static int scratchBytes;
        public static void Push(IntPtr L,string v)
        {
            if(v==null) { lua_pushnil(L);return; }
            int len=v.Length;
            if(len==0) { byte empty=0;PushBytes(L,&empty,0);return; }
            byte* temp=null;
            try {
                fixed(char* s=v) {
                    byte* o=Reserve(len,0,null,ref temp);
                    int n=NarrowAscii(s,o,len);
                    if(n<len) {
                        long need=n+(long)(len-n)*3;
                        if(need>int.MaxValue)throw new OverflowException("String result too large for Lua");
                        o=Reserve((int)need,n,o,ref temp);
                        n+=Encoding.UTF8.GetBytes(s+n,len-n,o+n,(int)need-n);
                    }
                    PushBytes(L,o,n);
                }
            } finally { if(temp!=null)Marshal.FreeHGlobal((IntPtr)temp); }
        }
        // 获取至少 need 字节的缓冲区，并保留 current 的前 keep 字节内容。
        static byte* Reserve(int need,int keep,byte* current,ref byte* temp)
        {
            if(need<=ScratchKeepBytes) {
                if(need<=scratchBytes)return scratch;
                int cap=scratchBytes==0 ? 256 : scratchBytes;
                while(cap<need)cap*=2;
                byte* p=(byte*)Marshal.AllocHGlobal(cap);
                if(keep>0)Buffer.MemoryCopy(current,p,cap,keep);
                if(scratch!=null)Marshal.FreeHGlobal((IntPtr)scratch);
                scratch=p;scratchBytes=cap;
                return p;
            }
            byte* t=(byte*)Marshal.AllocHGlobal(need);
            if(keep>0)Buffer.MemoryCopy(current,t,need,keep);
            if(temp!=null)Marshal.FreeHGlobal((IntPtr)temp);
            temp=t;
            return t;
        }
        // 可在两次调用之间释放共享缓冲；后续再次溢出时会重新分配。
        public static void ReleaseScratch()
        {
            if(scratch!=null) { Marshal.FreeHGlobal((IntPtr)scratch);scratch=null;scratchBytes=0; }
        }
    }
}
