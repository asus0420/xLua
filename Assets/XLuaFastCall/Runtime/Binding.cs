using System;
using System.Runtime.InteropServices;
using System.Text;
using XLua;
using LuaAPI=XLua.LuaDLL.Lua;
namespace XLuaFastCall {
// 每个 LuaEnv 对应一个优化绑定；原生实现编入各平台的 XLua 库，共用同一份 Lua 核心。
public sealed unsafe partial class Binding : IDisposable {
 [StructLayout(LayoutKind.Explicit,Size=16)] struct Slot { [FieldOffset(0)] public long Integer; [FieldOffset(0)] public double Number; [FieldOffset(0)] public ulong Address; [FieldOffset(8)] public int Tag; [FieldOffset(12)] public int Length; }
 [StructLayout(LayoutKind.Explicit,Size=32)] struct Cell { [FieldOffset(0)] public long Integer; [FieldOffset(0)] public double Number; [FieldOffset(0)] public ulong Address; [FieldOffset(8)] public long Length; [FieldOffset(16)] public long Size; }
 // 64 位字段置于首位，避免 32 位编译器对 int64 对齐规则不同导致字段偏移不一致。
 [StructLayout(LayoutKind.Sequential)] struct Frame { public long Receiver; public IntPtr Token; public Cell* Cells; public byte* Error; public int Count; }
 [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int Callback(IntPtr L,Slot* args,Frame* frame);
 // iOS 使用静态链接入口；Windows/Android 分别解析 xlua.dll 和 libxlua.so。
#if (UNITY_IOS || UNITY_IPHONE) && !UNITY_EDITOR
 const string Native="__Internal";
#else
 const string Native="xlua";
#endif
 [DllImport(Native,CallingConvention=CallingConvention.Cdecl)] static extern int sd_layout([Out] int[] values);
 [DllImport(Native,CallingConvention=CallingConvention.Cdecl)] static extern int sd_open(IntPtr L,IntPtr token);
 [DllImport(Native,CallingConvention=CallingConvention.Cdecl)] static extern int sd_install(IntPtr L,string name,IntPtr cb,int[] kinds,int count,int required,int allowExtra);
 [DllImport(Native,CallingConvention=CallingConvention.Cdecl)] static extern int sd_close(IntPtr L);
 // sd_layout 返回数组的下标，必须与 bridge.c 中的 L_* 枚举顺序一致。
 internal const int LayoutSlot=0,LayoutSlotTag=1,LayoutCell=2,LayoutSlotLength=3,LayoutFrameCells=4,LayoutFrameCount=5,
                    LayoutFrameError=6,LayoutCellKind=7,LayoutCellSize=8,LayoutFrameToken=9,LayoutFrame=10,LayoutFrameReceiver=11,LayoutCount=12;
 internal const int BufferAbiVersion=1002;
 // 自定义参数标签，与 bridge.c 一致；不再读取 Lua 内部标签。
 const int TagInteger=19,TagFloat=3,TagNil=0,TagBoolean=1;

 readonly LuaEnv env; readonly ObjectTranslator translator;
 readonly int thread=System.Threading.Thread.CurrentThread.ManagedThreadId;
 // 用数组槽位保存 Binding，维持绑定生命周期；需要访问解释器上下文时可通过 token 定位。
 static Binding[] bindings=new Binding[16];
 readonly int token; bool closed;

 // 注册逻辑只在初始化时执行，不参与每次优化调用。
 partial void InstallConfigured(ref int installed);
 public Binding(LuaEnv env) : this(env,null) { }
 internal Binding(LuaEnv env,Action<Binding> register) {
  this.env=env;translator=ObjectTranslatorPool.Instance.Find(env.L);
  var layout=new int[LayoutCount];
  // 同时检查字段偏移，避免加载旧 DLL 或使用不同对齐规则。
  int Off(string field)=>(int)Marshal.OffsetOf(typeof(Frame),field);
  if(sd_layout(layout)!=BufferAbiVersion
     || layout[LayoutSlot]!=sizeof(Slot) || layout[LayoutSlotTag]!=8 || layout[LayoutSlotLength]!=12
     || layout[LayoutCell]!=sizeof(Cell) || layout[LayoutCellKind]!=8 || layout[LayoutCellSize]!=16
     || layout[LayoutFrame]!=sizeof(Frame) || layout[LayoutFrameReceiver]!=Off(nameof(Frame.Receiver))
     || layout[LayoutFrameToken]!=Off(nameof(Frame.Token)) || layout[LayoutFrameCells]!=Off(nameof(Frame.Cells))
     || layout[LayoutFrameError]!=Off(nameof(Frame.Error)) || layout[LayoutFrameCount]!=Off(nameof(Frame.Count)))
   throw new InvalidOperationException("XLuaFastCall buffer ABI mismatch (rebuild xlua native library)");
  LuaStringCodec.Initialize();
  token=Add(this);
  bool opened=false;
  try {
   if(sd_open(env.L,(IntPtr)token)!=0)throw new InvalidOperationException("XLuaFastCall open failed");
   opened=true;
   if(register!=null)register(this);
   else { int installed=0;InstallConfigured(ref installed);if(installed==0)throw new InvalidOperationException("Run XLua/Generate Code before creating a configured binding"); }
  } catch { if(opened)sd_close(env.L);bindings[token-1]=null;throw; }
 }
 static int Add(Binding value) {
  for(int i=0;i<bindings.Length;i++)if(bindings[i]==null) { bindings[i]=value;return i+1; }
  int index=bindings.Length;Array.Resize(ref bindings,index*2);bindings[index]=value;return index+1;
 }
 void Register(string name,Callback cb,int[] kinds,int required,bool allowExtra) {
  if(sd_install(env.L,name,Marshal.GetFunctionPointerForDelegate(cb),kinds,kinds.Length,required,allowExtra?1:0)!=0)throw new InvalidOperationException("Registration failed: "+name);
 }
 void Check() {
  if(closed)throw new ObjectDisposedException(nameof(Binding));
  if(thread!=System.Threading.Thread.CurrentThread.ManagedThreadId)throw new InvalidOperationException("Use owning Lua thread");
 }
 public void Dispose() {
  if(closed)return;Check();
  if(sd_close(env.L)!=0)throw new InvalidOperationException("Active callback");
  closed=true;bindings[token-1]=null;
  LuaStringCodec.ReleaseScratch();
 }

 // C 用 Lua 内部宏读栈并填入固定宽度 Slot，C# 不读取 Lua 内部布局。
 static long Integer(Slot* args,Cell* cells,int i) {
  if(args[i].Tag==TagInteger)return args[i].Integer;
  if(args[i].Tag==TagFloat) { double n=args[i].Number;return n>=long.MinValue && n<9223372036854775808.0 && Math.Floor(n)==n?(long)n:0; }
  return cells[i].Integer;
 }
 static bool Boolean(Slot* args,int i)=>args[i].Tag!=TagNil && (args[i].Tag!=TagBoolean || *(int*)(args+i)!=0);
 static float Float(Slot* args,int i) {
  if(args[i].Tag==TagInteger)return args[i].Integer;
  if(args[i].Tag==TagFloat)return (float)args[i].Number;
  throw new InvalidCastException("Expected Lua number");
 }
 static double StrictNumber(Slot* args,int i) {
  if(args[i].Tag==TagInteger)return args[i].Integer;
  if(args[i].Tag==TagFloat)return args[i].Number;
  throw new InvalidCastException("Expected Lua number");
 }
 // C 提供字符串内容地址和字节长度；托管端不读取 TString 头。
 static string ReadString(Slot* args,int i)=>LuaStringCodec.Read((byte*)args[i].Address,args[i].Length);


 // 结果写入：Cell.Length 表示返回类型，1=整数、2=浮点数、3=布尔值、6=字符串。
 static void Write(Cell* p,int v) { p->Integer=v;p->Length=1; }
 static void Write(Cell* p,byte v) { p->Integer=v;p->Length=1; }
 static void Write(Cell* p,long v) { p->Integer=v;p->Length=1; }
 static void Write(Cell* p,float v) { p->Number=v;p->Length=2; }
 static void Write(Cell* p,double v) { p->Number=v;p->Length=2; }
 static void Write(Cell* p,bool v) { p->Integer=v?1:0;p->Length=3; }
 // 将字符串编码到当前调用深度的缓冲区；多个字符串通过 used 共用空间，由 C 统一入栈。
 // 返回 false 表示 UTF8 长度上界超出缓冲区，回调需自行入栈全部结果并返回 100+结果数。
 static bool WriteString(Cell* p,string v,byte* buffer,ref int used) {
  p->Length=6;
  if(!LuaStringCodec.Encode(v,buffer,ref used,out byte* output,out long size))return false;
  p->Address=(ulong)output;p->Size=size;return true;
 }
 static int Error(Exception e,byte* buffer) {
  byte[] bytes=Encoding.UTF8.GetBytes(e.ToString());int n=Math.Min(4095,bytes.Length);
  Marshal.Copy(bytes,0,(IntPtr)buffer,n);buffer[n]=0;return -1;
 }
}
}
