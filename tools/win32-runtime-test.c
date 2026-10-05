/* Genuine Kernel32 threading, TLS, file and process test. MIT license.
 * CRT-free so the first Wine bringup test only requires kernel32/ntdll.
 * Uses an isolated temporary file, deletes it, then ExitProcess(0) on success.
 * It must run through a complete Windows runtime, never the bounded PE smoke. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static DWORD tls_index;
static volatile LONG thread_ok;
static DWORD WINAPI worker(void* argument) {
    if(TlsGetValue(tls_index)!=NULL) return 1;
    if(!TlsSetValue(tls_index,argument) || TlsGetValue(tls_index)!=argument) return 2;
    InterlockedExchange(&thread_ok,1);
    return 42;
}
static void finish(UINT status) {
    /* which check failed, and the error it left: the exit status is not visible everywhere */
    if(status) {
        char line[] = "WoWPS5 Win32 runtime check 00 error 00000000\r\n";
        const DWORD error = GetLastError(); DWORD count;
        line[27] = (char)('0' + status / 10); line[28] = (char)('0' + status % 10);
        for(int i = 0; i < 8; i++) line[36 + i] = "0123456789abcdef"[(error >> (28 - 4 * i)) & 15];
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, sizeof(line) - 1, &count, NULL);
    }
    static const char pass[]="WoWPS5 Win32 runtime test PASS\r\n";
    static const char fail[]="WoWPS5 Win32 runtime test FAIL\r\n";
    const char* message=status ? fail:pass;
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),message,sizeof(pass)-1,&written,NULL);
    ExitProcess(status);
}
void WoWPS5Win32Test(void) {
    tls_index=TlsAlloc();
    if(tls_index==TLS_OUT_OF_INDEXES || !TlsSetValue(tls_index,(void*)123)) finish(10);
    HANDLE thread=CreateThread(NULL,0,worker,(void*)456,0,NULL);
    if(!thread) finish(11);
    DWORD thread_status=0;
    if(WaitForSingleObject(thread,10000)!=WAIT_OBJECT_0 || !GetExitCodeThread(thread,&thread_status)) finish(12);
    CloseHandle(thread);
    if(thread_status!=42 || thread_ok!=1 || TlsGetValue(tls_index)!=(void*)123) finish(13);
    if(!TlsFree(tls_index)) finish(14);
    WCHAR directory[MAX_PATH], path[MAX_PATH];
    DWORD size=GetTempPathW(MAX_PATH,directory);
    if(!size || size>=MAX_PATH || !GetTempFileNameW(directory,L"w5t",0,path)) finish(20);
    HANDLE file=CreateFileW(path,GENERIC_READ|GENERIC_WRITE,0,NULL,TRUNCATE_EXISTING,FILE_ATTRIBUTE_TEMPORARY,NULL);
    if(file==INVALID_HANDLE_VALUE) { DeleteFileW(path); finish(21); }
    const DWORD expected=0x57505335; DWORD actual=0,count=0;
    BOOL ok=WriteFile(file,&expected,sizeof(expected),&count,NULL) && count==sizeof(expected);
    if(ok) ok=SetFilePointer(file,0,NULL,FILE_BEGIN)!=INVALID_SET_FILE_POINTER;
    if(ok) ok=ReadFile(file,&actual,sizeof(actual),&count,NULL) && count==sizeof(actual) && actual==expected;
    CloseHandle(file);
    if(!DeleteFileW(path)) finish(22);
    if(!ok) finish(23);
    finish(0);
}
