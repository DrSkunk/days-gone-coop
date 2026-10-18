/* Diagnostic bootstrap only: no hooks, network, gameplay changes or save access. */
typedef unsigned long DWORD;
typedef unsigned long long U64;
typedef unsigned short WCHAR;
typedef void *HANDLE;
#define API __declspec(dllimport)
API HANDLE __stdcall GetModuleHandleW(const WCHAR*);
API DWORD __stdcall GetModuleFileNameW(HANDLE,WCHAR*,DWORD);
API HANDLE __stdcall CreateFileW(const WCHAR*,DWORD,DWORD,void*,DWORD,DWORD,HANDLE);
API int __stdcall WriteFile(HANDLE,const void*,DWORD,DWORD*,void*);
API int __stdcall CloseHandle(HANDLE);
API DWORD __stdcall GetCurrentProcessId(void);
API U64 __stdcall GetTickCount64(void);
API int __stdcall DisableThreadLibraryCalls(HANDLE);
static HANDLE self;
static volatile long initialized;
static HANDLE log_file;
static void write_text(const char *s) {
    DWORD len=0, written=0;
    while(s[len]) ++len;
    WriteFile(log_file,s,len,&written,0);
}
static void hex(const char *name,U64 x) {
    char b[19]; const char *digits="0123456789abcdef";
    b[0]='0'; b[1]='x'; b[18]=0;
    for(int i=17;i>=2;--i) { b[i]=digits[x&15]; x>>=4; }
    write_text(name);write_text(b);write_text("\r\n");
}
__declspec(dllexport) void InitializeASI(void) {
    if (__sync_lock_test_and_set(&initialized,1)) return;
    WCHAR path[1024];
    DWORD n=GetModuleFileNameW(self,path,1024);
    if(!n || n>=1024) return;
    while(n && path[n-1]!='\\' && path[n-1]!='/') --n;
    const char *tail="DaysGoneCoopProbe.log";
    unsigned i=0; while(tail[i] && n+i<1023) {path[n+i]=(WCHAR)tail[i];++i;}
    path[n+i]=0;
    log_file=CreateFileW(path,0x40000000,1,0,2,0x80,0);
    if(log_file==(HANDLE)(U64)-1) return;
    write_text("Days Gone co-op diagnostic probe v0.1\r\nStatus: loader entered InitializeASI; this is NOT a multiplayer mod.\r\n");
    hex("process_id=",GetCurrentProcessId());
    hex("system_uptime_ms=",GetTickCount64());
    unsigned char *base=(unsigned char*)GetModuleHandleW(0);
    hex("main_module_base=",(U64)base);
    if(base && base[0]=='M' && base[1]=='Z') {
        DWORD off=*(DWORD*)(base+0x3c);
        if(off>=64 && off<0x1000 && *(DWORD*)(base+off)==0x4550) {
            hex("machine=",*(unsigned short*)(base+off+4));
            hex("pe_timestamp=",*(DWORD*)(base+off+8));
            hex("image_size=",*(DWORD*)(base+off+24+56));
            write_text("main_image_headers=valid\r\n");
        }
    }
    write_text("bootstrap=complete\r\nGameplay hooks, actor spawning and replication remain unimplemented.\r\n");
    CloseHandle(log_file);
}
int __stdcall DllMain(HANDLE module,DWORD reason,void *reserved) {
    (void)reserved;
    if(reason==1) {self=module;DisableThreadLibraryCalls(module);}
    return 1;
}
