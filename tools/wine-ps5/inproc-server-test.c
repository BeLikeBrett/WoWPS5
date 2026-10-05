/* Exercises what changes when wineserver is a thread of its one client:
 * signals the server sends to client threads (suspend, terminate), server access to
 * client memory, anonymous mappings, opens relative to a directory handle, registry
 * persistence across runs, and child-process creation, which must fail with an error.
 * CRT-free: kernel32 and ntdll only. MIT license.
 *
 *   x86_64-w64-mingw32-gcc -O2 -nostdlib -fno-stack-protector -fno-builtin \
 *       -Wl,--entry,InprocTest -Wl,--enable-reloc-section inproc-server-test.c \
 *       -lkernel32 -lntdll -o inproc-server-test.exe
 *
 * Exit code 0 means every check passed; each check prints one line. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>

NTSTATUS NTAPI NtCreateKey(HANDLE*,ACCESS_MASK,OBJECT_ATTRIBUTES*,ULONG,UNICODE_STRING*,ULONG,ULONG*);
NTSTATUS NTAPI NtSetValueKey(HANDLE,UNICODE_STRING*,ULONG,ULONG,const void*,ULONG);
NTSTATUS NTAPI NtQueryValueKey(HANDLE,UNICODE_STRING*,int,void*,ULONG,ULONG*);
NTSTATUS NTAPI NtFlushKey(HANDLE);

static HANDLE out;
static int failures;

static void put(const char* text) {
    DWORD length=0, written;
    while(text[length]) length++;
    WriteFile(out,text,length,&written,NULL);
}
static void put_hex(ULONG_PTR value) {
    char buffer[19]; int i;
    buffer[0]='0'; buffer[1]='x';
    for(i=0;i<16;i++) buffer[2+i]="0123456789abcdef"[(value>>(60-4*i))&15];
    buffer[18]=0;
    put(buffer);
}
static void check(const char* name, BOOL ok, ULONG_PTR detail) {
    put(ok ? "ok   " : "FAIL ");
    put(name); put(" ("); put_hex(detail); put(")\r\n");
    if(!ok) failures++;
}
static void init_string(UNICODE_STRING* string, const WCHAR* text) {
    USHORT length=0;
    while(text[length]) length++;
    string->Buffer=(WCHAR*)text;
    string->Length=length*sizeof(WCHAR);
    string->MaximumLength=string->Length+sizeof(WCHAR);
}

static volatile LONG spin_count, spin_stop;
static DWORD WINAPI spinner(void* argument) {
    (void)argument;
    while(!spin_stop) InterlockedIncrement(&spin_count);
    return 7;
}
static DWORD WINAPI sleeper(void* argument) {
    (void)argument;
    Sleep(INFINITE);
    return 9;
}

void InprocTest(void) {
    out=GetStdHandle(STD_OUTPUT_HANDLE);

    /* 1. suspend: the server signals the thread (SIGUSR1) and takes its context */
    HANDLE thread=CreateThread(NULL,0,spinner,NULL,0,NULL);
    check("CreateThread spinner",thread!=NULL,GetLastError());
    while(spin_count<1000) Sleep(1);
    DWORD previous=SuspendThread(thread);
    check("SuspendThread",previous==0,previous);
    CONTEXT context;
    context.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER;
    BOOL got=GetThreadContext(thread,&context);
    check("GetThreadContext of suspended thread",got && context.Rip!=0,got ? context.Rip : GetLastError());
    LONG before=spin_count; Sleep(50); LONG after=spin_count;
    check("suspended thread makes no progress",before==after,(ULONG_PTR)(after-before));

    /* 2. hardware breakpoints need a tracer process: must be refused, not ignored */
    context.ContextFlags=CONTEXT_DEBUG_REGISTERS;
    got=GetThreadContext(thread,&context);
    check("GetThreadContext debug registers",got && context.Dr7==0,got ? context.Dr7 : GetLastError());
    context.ContextFlags=CONTEXT_DEBUG_REGISTERS;
    context.Dr0=(ULONG_PTR)&spin_count; context.Dr7=0x1;
    SetLastError(0);
    BOOL set=SetThreadContext(thread,&context);
    check("SetThreadContext debug registers is refused",!set,GetLastError());

    previous=ResumeThread(thread);
    check("ResumeThread",previous==1,previous);
    before=spin_count; Sleep(50); after=spin_count;
    check("resumed thread makes progress",after!=before,(ULONG_PTR)(after-before));
    spin_stop=1;
    DWORD code=0;
    check("spinner exits",WaitForSingleObject(thread,10000)==WAIT_OBJECT_0 && GetExitCodeThread(thread,&code) && code==7,code);
    CloseHandle(thread);

    /* 3. terminate: the server signals the thread (SIGQUIT) */
    thread=CreateThread(NULL,0,sleeper,NULL,0,NULL);
    Sleep(50);
    BOOL terminated=TerminateThread(thread,33);
    code=0;
    check("TerminateThread",terminated && WaitForSingleObject(thread,10000)==WAIT_OBJECT_0 &&
          GetExitCodeThread(thread,&code) && code==33,terminated ? code : GetLastError());
    CloseHandle(thread);

    /* 4. the server reads and writes client memory through a real process handle */
    HANDLE process=OpenProcess(PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION,FALSE,GetCurrentProcessId());
    check("OpenProcess self",process!=NULL,GetLastError());
    static volatile DWORD source=0x57505335, target;
    DWORD copy=0; SIZE_T count=0;
    BOOL ok=ReadProcessMemory(process,(void*)&source,&copy,sizeof(copy),&count);
    check("ReadProcessMemory",ok && count==sizeof(copy) && copy==0x57505335,ok ? copy : GetLastError());
    DWORD value=0x12345678; count=0;
    ok=WriteProcessMemory(process,(void*)&target,&value,sizeof(value),&count);
    check("WriteProcessMemory",ok && count==sizeof(value) && target==0x12345678,ok ? target : GetLastError());
    SetLastError(0);
    ok=ReadProcessMemory(process,(void*)(ULONG_PTR)0x10,&copy,sizeof(copy),&count);
    check("ReadProcessMemory of an unmapped address fails",!ok,GetLastError());
    void* page=VirtualAlloc(NULL,0x10000,MEM_RESERVE|MEM_COMMIT,PAGE_READONLY);
    SetLastError(0);
    ok=WriteProcessMemory(process,page,&value,sizeof(value),&count);
    check("WriteProcessMemory to a read-only page fails",page && !ok,GetLastError());
    ok=ReadProcessMemory(process,page,&copy,sizeof(copy),&count);
    check("ReadProcessMemory of a read-only page",ok && copy==0,ok ? copy : GetLastError());
    VirtualFree(page,0,MEM_RELEASE);
    CloseHandle(process);

    /* 5. anonymous mapping: the server creates its backing file */
    HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,0x20000,NULL);
    check("CreateFileMapping anonymous",mapping!=NULL,GetLastError());
    volatile DWORD* view=mapping ? MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,0) : NULL;
    volatile DWORD* second=mapping ? MapViewOfFile(mapping,FILE_MAP_READ,0,0,0) : NULL;
    if(view) view[0x4000]=0xfeedf00d;
    check("two views of the mapping share memory",view && second && second[0x4000]==0xfeedf00d,(ULONG_PTR)view);
    if(view) UnmapViewOfFile((void*)view);
    if(second) UnmapViewOfFile((void*)second);
    if(mapping) CloseHandle(mapping);

    /* 6. open relative to a directory handle: the server joins the paths */
    WCHAR directory[MAX_PATH];
    DWORD size=GetTempPathW(MAX_PATH,directory);
    HANDLE root=CreateFileW(directory,FILE_LIST_DIRECTORY|FILE_TRAVERSE|SYNCHRONIZE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                            NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
    check("open temp directory",size && root!=INVALID_HANDLE_VALUE,GetLastError());
    UNICODE_STRING name; OBJECT_ATTRIBUTES attributes; IO_STATUS_BLOCK io; HANDLE file=NULL;
    init_string(&name,L"wowps5-inproc-relative.tmp");
    InitializeObjectAttributes(&attributes,&name,OBJ_CASE_INSENSITIVE,root,NULL);
    NTSTATUS status=NtCreateFile(&file,GENERIC_READ|GENERIC_WRITE|DELETE|SYNCHRONIZE,&attributes,&io,NULL,FILE_ATTRIBUTE_NORMAL,
                                 0,FILE_OVERWRITE_IF,FILE_NON_DIRECTORY_FILE|FILE_SYNCHRONOUS_IO_NONALERT|FILE_DELETE_ON_CLOSE,NULL,0);
    check("NtCreateFile relative to a directory handle",status==0,(ULONG)status);
    DWORD written=0;
    if(!status) {
        ok=WriteFile(file,&value,sizeof(value),&written,NULL);
        check("write to the relative file",ok && written==sizeof(value),GetLastError());
        WCHAR path[MAX_PATH]; DWORD i=0,j=0;
        while(directory[i]) { path[i]=directory[i]; i++; }
        while(name.Buffer[j]) path[i++]=name.Buffer[j++];
        path[i]=0;
        check("the file exists under the directory",GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES,GetLastError());
        CloseHandle(file);
        check("delete-on-close removed it",GetFileAttributesW(path)==INVALID_FILE_ATTRIBUTES,GetLastError());
    }
    CloseHandle(root);

    /* 7. a child process cannot exist: creation must fail with an error */
    /* (this very program is used as the image so that the failure is not "file not found") */
    STARTUPINFOW startup; PROCESS_INFORMATION info; static WCHAR self[MAX_PATH];
    for(size=0;size<sizeof(startup);size++) ((volatile char*)&startup)[size]=0;
    startup.cb=sizeof(startup);
    size=GetModuleFileNameW(NULL,self,MAX_PATH);
    SetLastError(0);
    ok=CreateProcessW(self,NULL,NULL,NULL,FALSE,0,NULL,NULL,&startup,&info);
    check("CreateProcess of an existing image fails with ERROR_NOT_SUPPORTED",
          size && !ok && GetLastError()==ERROR_NOT_SUPPORTED,GetLastError());

    /* 8. registry: a value written by the previous run must have been saved at its exit */
    HANDLE key=NULL; ULONG disposition=0;
    init_string(&name,L"\\Registry\\Machine\\Software\\WoWPS5InprocTest");
    InitializeObjectAttributes(&attributes,&name,OBJ_CASE_INSENSITIVE,NULL,NULL);
    status=NtCreateKey(&key,KEY_ALL_ACCESS,&attributes,0,NULL,0,&disposition);
    check("NtCreateKey",status==0,(ULONG)status);
    if(!status) {
        struct { ULONG title, type, length; DWORD data; } partial;
        ULONG needed=0; DWORD runs=0;
        init_string(&name,L"Runs");
        if(!NtQueryValueKey(key,&name,2 /* KeyValuePartialInformation */,&partial,sizeof(partial),&needed)) runs=partial.data;
        put(disposition==1 ? "info registry key created by this run, Runs=" : "info registry key found from an earlier run, Runs=");
        put_hex(runs); put("\r\n");
        check("registry key state matches the saved run count",(disposition==1)==(runs==0),runs);
        runs++;
        status=NtSetValueKey(key,&name,0,REG_DWORD,&runs,sizeof(runs));
        check("NtSetValueKey",status==0,(ULONG)status);
        CloseHandle(key);
    }

    /* 9. exit with another thread still running: the server terminates it (SIGQUIT)
     * and the registry is saved before the process ends */
    spin_stop=0;
    thread=CreateThread(NULL,0,spinner,NULL,0,NULL);
    Sleep(20);
    put(failures ? "WoWPS5 in-process server test FAIL\r\n" : "WoWPS5 in-process server test PASS\r\n");
    ExitProcess(failures);
}
