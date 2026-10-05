/* Windows virtual-memory behaviour a game depends on. MIT license.
 * CRT-free: Kernel32 only. Each check prints PASS or FAIL; the exit status is
 * the number of failures. Run under stock Wine for the reference, under the
 * memory-model build for the console's rules (16 KiB host pages, direct
 * memory), and through the native runtime on the console.
 * The 4 KiB cases matter most: Windows protects single 4 KiB pages, and a host
 * with 16 KiB pages can only do that when Wine emulates it. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static HANDLE out;
static UINT failures;
static volatile LONG faults, guard_faults;
static volatile ULONG_PTR fault_address;
static volatile DWORD repair_protection;

void* memset(void* target, int value, size_t count) {
    volatile unsigned char* p = target;
    while(count--) *p++ = (unsigned char)value;
    return target;
}
/* The accesses under test go through these, so the compiler cannot move one
 * across the volatile stores that arm the handler. */
static __attribute__((noinline)) void poke(volatile unsigned char* address, unsigned char value) { *address = value; }
static __attribute__((noinline)) unsigned char peek(const volatile unsigned char* address) { return *address; }
static void print(const char* text) {
    DWORD length = 0, written;
    while(text[length]) length++;
    WriteFile(out, text, length, &written, NULL);
}
static void check(BOOL passed, const char* what) {
    if(!passed) failures++;
    print(passed ? "PASS: " : "FAIL: "); print(what); print("\r\n");
}

/* A section whose pages are sealed: inaccessible, and holding the wrong code,
 * until the first access to each raises an exception and the handler puts the
 * right code there through a second view. A protected executable keeps its
 * code this way, and reads its own header while it does. */
static unsigned char *sealed_runs, *sealed_writes;
static volatile LONG sealed_opened, sealed_header_misread;
static void sealed_code(unsigned page, unsigned char value) {
    static const unsigned char body[] = { 0xb8, 0, 0, 0, 0, 0xc3 };          /* mov eax,value; ret */
    for(unsigned i = 0; i < sizeof(body); i++) sealed_writes[page * 0x1000 + 0x800 + i] = body[i];
    sealed_writes[page * 0x1000 + 0x801] = value;
}
static __attribute__((noinline)) int sealed_call(unsigned page) { return ((int(*)(void))(sealed_runs + page * 0x1000 + 0x800))(); }

/* An access violation is repaired by restoring the page's protection and rerunning. */
static LONG CALLBACK handler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    if(record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && sealed_runs &&
       record->ExceptionInformation[1] - (ULONG_PTR)sealed_runs < 0x10000) {
        const unsigned page = (unsigned)((record->ExceptionInformation[1] - (ULONG_PTR)sealed_runs) >> 12);
        DWORD old;
        if(!page || peek(sealed_runs + 0x10) != 0x4d) InterlockedIncrement(&sealed_header_misread);
        sealed_code(page, (unsigned char)page);
        InterlockedOr(&sealed_opened, 1 << page);
        return VirtualProtect(sealed_runs + page * 0x1000, 0x1000, PAGE_EXECUTE_READ, &old) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    }
    if(record->ExceptionCode == STATUS_GUARD_PAGE_VIOLATION) {
        InterlockedIncrement(&guard_faults);
        return EXCEPTION_CONTINUE_EXECUTION;       /* the guard is gone after its one fault */
    }
    if(record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !repair_protection) return EXCEPTION_CONTINUE_SEARCH;
    DWORD old;
    fault_address = record->ExceptionInformation[1];
    InterlockedIncrement(&faults);
    if(!VirtualProtect((void*)(fault_address & ~(ULONG_PTR)0xfff), 0x1000, repair_protection, &old)) return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD protection(const void* address) {
    MEMORY_BASIC_INFORMATION info;
    return VirtualQuery(address, &info, sizeof(info)) ? info.Protect : 0;
}
static DWORD WINAPI deep(void* argument) {
    volatile char frame[0x30000];                  /* 192 KiB: grows the stack through its guard pages */
    for(unsigned at = 0; at < sizeof(frame); at += 0x1000) frame[sizeof(frame)-1-at] = (char)at;
    return argument ? frame[sizeof(frame)-1] + 7 : 0;
}

void WoWPS5MemoryTest(void) {
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    SYSTEM_INFO system; GetSystemInfo(&system);
    check(system.dwPageSize == 0x1000 && system.dwAllocationGranularity == 0x10000, "Windows reports 4 KiB pages and 64 KiB granularity");
    AddVectoredExceptionHandler(1, handler);

    /* reserve, then commit single 4 KiB pages */
    unsigned char* base = VirtualAlloc(NULL, 0x100000, MEM_RESERVE, PAGE_NOACCESS);
    check(base && !((ULONG_PTR)base & 0xffff), "a reservation is 64 KiB aligned");
    unsigned char* page = VirtualAlloc(base + 0x5000, 0x1000, MEM_COMMIT, PAGE_READWRITE);
    BOOL zero = page != NULL;
    for(unsigned i = 0; zero && i < 0x1000; i++) zero = page[i] == 0;
    check(page == base + 0x5000 && zero, "one committed 4 KiB page is zero");
    if(page) memset(page, 0x77, 0x1000);
    MEMORY_BASIC_INFORMATION info;
    check(VirtualQuery(base + 0x4000, &info, sizeof(info)) && info.State == MEM_RESERVE &&
          VirtualQuery(base + 0x5000, &info, sizeof(info)) && info.State == MEM_COMMIT && info.RegionSize == 0x1000,
          "its neighbours in the same 16 KiB stay reserved, as VirtualQuery reports");

    /* a read-only 4 KiB page between writable ones: the write must fault, and be repairable */
    unsigned char* trio = VirtualAlloc(base + 0x10000, 0x4000, MEM_COMMIT, PAGE_READWRITE);
    DWORD old = 0;
    BOOL ok = trio && VirtualProtect(trio + 0x1000, 0x1000, PAGE_READONLY, &old) && old == PAGE_READWRITE;
    check(ok && protection(trio + 0x1000) == PAGE_READONLY && protection(trio) == PAGE_READWRITE && protection(trio + 0x2000) == PAGE_READWRITE,
          "VirtualProtect on one 4 KiB page leaves the pages around it writable");
    if(ok) {
        const LONG before = faults;
        poke(trio, 1); poke(trio + 0x2000, 3);     /* neighbours: no fault, with no handler armed */
        repair_protection = PAGE_READWRITE;
        poke(trio + 0x1800, 2);                    /* faults once; the handler makes the page writable */
        repair_protection = 0;
        check(faults == before + 1 && fault_address == (ULONG_PTR)trio + 0x1800 && peek(trio + 0x1800) == 2 && peek(trio) == 1 && peek(trio + 0x2000) == 3,
              "a write to a read-only 4 KiB page faults at its address and reruns after the handler repairs it");
    }
    /* no access on one 4 KiB page */
    ok = trio && VirtualProtect(trio + 0x3000, 0x1000, PAGE_NOACCESS, &old);
    if(ok) {
        const LONG before = faults;
        repair_protection = PAGE_READONLY;
        const unsigned char value = peek(trio + 0x3004);
        repair_protection = 0;
        check(faults == before + 1 && value == 0 && protection(trio + 0x3000) == PAGE_READONLY, "a read of a no-access 4 KiB page faults and is repaired");
    } else check(FALSE, "PAGE_NOACCESS on one 4 KiB page");

    /* a guard page raises its exception once */
    unsigned char* guard = VirtualAlloc(base + 0x20000, 0x3000, MEM_COMMIT, PAGE_READWRITE);
    ok = guard && VirtualProtect(guard + 0x1000, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old);
    if(ok) {
        const LONG before = guard_faults;
        poke(guard, 1); poke(guard + 0x2000, 1);   /* not guarded */
        check(guard_faults == before, "the pages beside a 4 KiB guard page are not guarded");
        poke(guard + 0x1010, 9); poke(guard + 0x1020, 9);   /* the first raises, the second does not */
        check(guard_faults == before + 1 && peek(guard + 0x1010) == 9 && protection(guard + 0x1000) == PAGE_READWRITE, "a 4 KiB guard page raises once and then behaves as ordinary memory");
    } else check(FALSE, "PAGE_GUARD on one 4 KiB page");

    /* decommit and recommit reads zero; MEM_RESET keeps the page committed */
    ok = page && VirtualFree(page, 0x1000, MEM_DECOMMIT) && VirtualAlloc(page, 0x1000, MEM_COMMIT, PAGE_READWRITE) == page;
    check(ok && page[0] == 0 && page[0xfff] == 0, "a decommitted page comes back zero");
    check(trio && VirtualAlloc(trio, 0x1000, MEM_RESET, PAGE_READWRITE) == trio && protection(trio) == PAGE_READWRITE, "MEM_RESET leaves the page committed");

    /* code: written, run, protected read-execute, run again */
    unsigned char* code = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    int first = -1, second = -1;
    if(code) {
        static const unsigned char body[] = { 0xb8, 0x2a, 0, 0, 0, 0xc3 };   /* mov eax,42; ret */
        for(unsigned i = 0; i < sizeof(body); i++) code[i] = body[i];
        first = ((int(*)(void))code)();
        code[1] = 0x2b;
        FlushInstructionCache(GetCurrentProcess(), code, sizeof(body));
        if(VirtualProtect(code, 0x1000, PAGE_EXECUTE_READ, &old)) second = ((int(*)(void))code)();
    }
    check(first == 42 && second == 43, "generated code runs, is rewritten in place, and runs read-execute");

    /* a pagefile-backed section mapped twice shares its pages */
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 0x30000, NULL);
    unsigned char* one = section ? MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, 0) : NULL;
    unsigned char* two = section ? MapViewOfFile(section, FILE_MAP_READ, 0, 0x10000, 0x10000) : NULL;
    if(one) one[0x10005] = 0x5a;
    check(one && two && two[5] == 0x5a, "two views of one section see the same memory");
    if(two) UnmapViewOfFile(two);
    if(one) UnmapViewOfFile(one);
    if(section) CloseHandle(section);

    /* the same with one view that runs and one that is written: how a program
     * makes code without ever holding writable and executable memory */
    section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE, 0, 0x100000, NULL);
    unsigned char* runs = section ? MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_EXECUTE, 0, 0, 0x100000) : NULL;
    unsigned char* writes = section ? MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0x100000) : NULL;
    first = second = -1;
    if(runs && writes) {
        static const unsigned char body[] = { 0xb8, 0x2a, 0, 0, 0, 0xc3 };
        for(unsigned i = 0; i < sizeof(body); i++) writes[0x3a10 + i] = body[i];
        if(peek(runs + 0x3a10) == 0xb8) first = ((int(*)(void))(runs + 0x3a10))();
        writes[0x3a11] = 0x2b;
        if(first == 42) second = ((int(*)(void))(runs + 0x3a10))();
    }
    check(runs && writes, "a section maps once to run and once to write");
    check(first == 42 && second == 43, "code written through one view of a section runs through the other");
    if(runs) UnmapViewOfFile(runs);
    if(writes) UnmapViewOfFile(writes);
    if(section) CloseHandle(section);

    /* sealed pages: each is put right by the exception its first access raises */
    section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE, 0, 0x10000, NULL);
    sealed_writes = section ? MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0x10000) : NULL;
    runs = section && sealed_writes ? MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_EXECUTE, 0, 0, 0x10000) : NULL;
    if(runs) {
        sealed_writes[0x10] = 0x4d;
        ok = VirtualProtect(runs, 0x1000, PAGE_READONLY, &old);
        for(unsigned page = 1; page < 16; page++) {
            sealed_code(page, 0xee);                                             /* what must never run */
            ok = ok && VirtualProtect(runs + page * 0x1000, 0x1000, PAGE_NOACCESS, &old);
        }
        sealed_runs = runs;
        const int five = sealed_call(5);
        const LONG after_five = sealed_opened;
        const int six = sealed_call(6);
        /* the handler reads page 0: with 16 KiB host pages that unseals pages 1 to 3 as well */
        check(ok && five == 5 && six == 6 && (after_five & (1 << 5)) && !(after_five >> 8),
              "sealed code pages are put right before they run, and no further than the 16 KiB around those touched");
        const unsigned char header = peek(runs + 0x10);
        const int one = sealed_call(1), nine = sealed_call(9);
        check(header == 0x4d && one == 1 && nine == 9 && !sealed_header_misread,
              "the readable page beside sealed ones reads, from the handler as well");
        check(!(sealed_opened >> 12) && protection(runs + 12 * 0x1000) == PAGE_NOACCESS && protection(runs + 6 * 0x1000) == PAGE_EXECUTE_READ,
              "sealed pages that were not reached stay sealed");
        sealed_runs = NULL;
        UnmapViewOfFile(runs);
    } else check(FALSE, "a section for sealed pages");
    if(sealed_writes) UnmapViewOfFile(sealed_writes);
    if(section) CloseHandle(section);

    /* a file mapped for reading, at an offset, and written through a second view */
    WCHAR directory[MAX_PATH], path[MAX_PATH];
    ok = GetTempPathW(MAX_PATH, directory) && GetTempFileNameW(directory, L"w5m", 0, path);
    HANDLE file = ok ? CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL) : INVALID_HANDLE_VALUE;
    if(file != INVALID_HANDLE_VALUE) {
        static unsigned char block[0x1000]; DWORD written;
        for(unsigned i = 0; i < 0x21; i++) { memset(block, (int)i, sizeof(block)); WriteFile(file, block, sizeof(block), &written, NULL); }
        HANDLE mapping = CreateFileMappingW(file, NULL, PAGE_READWRITE, 0, 0, NULL);
        unsigned char* reading = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0x10000, 0) : NULL;
        unsigned char* writing = mapping ? MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0x1000) : NULL;
        check(reading && reading[0] == 0x10 && reading[0xfff] == 0x10 && reading[0x10000] == 0x20, "a file view at a 64 KiB offset reads the file, including its odd-sized tail");
        if(writing) { writing[1] = 0xee; FlushViewOfFile(writing, 0x1000); UnmapViewOfFile(writing); }
        if(reading) UnmapViewOfFile(reading);
        if(mapping) CloseHandle(mapping);
        DWORD got = 0; unsigned char bytes[2] = {0, 0};
        SetFilePointer(file, 0, NULL, FILE_BEGIN);
        check(writing && ReadFile(file, bytes, 2, &got, NULL) && got == 2 && bytes[0] == 0 && bytes[1] == 0xee, "a write through a file view reaches the file");
        CloseHandle(file); DeleteFileW(path);
    } else check(FALSE, "a temporary file for the mapping checks");

    /* a thread whose frames outgrow the first committed part of its stack */
    DWORD status = 0;
    HANDLE thread = CreateThread(NULL, 0x100000, deep, (void*)1, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    ok = thread && WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0 && GetExitCodeThread(thread, &status);
    check(ok && status == 7, "a thread's stack grows through its guard pages");
    if(thread) CloseHandle(thread);

    /* a large region: 2 GiB reserved, 256 MiB of it committed and touched */
    unsigned char* large = VirtualAlloc(NULL, (SIZE_T)2 << 30, MEM_RESERVE, PAGE_NOACCESS);
    unsigned char* part = large ? VirtualAlloc(large + ((SIZE_T)1 << 30), 256u << 20, MEM_COMMIT, PAGE_READWRITE) : NULL;
    ok = part != NULL;
    for(SIZE_T at = 0; ok && at < (256u << 20); at += 0x1000) part[at] = (unsigned char)(at >> 12);
    for(SIZE_T at = 0; ok && at < (256u << 20); at += 0x100000) ok = part[at] == (unsigned char)(at >> 12);
    check(ok, "256 MiB committed inside a 2 GiB reservation");
    check(large && VirtualFree(large, 0, MEM_RELEASE) && VirtualFree(base, 0, MEM_RELEASE), "regions release");

    /* the heap, which sits on all of the above */
    HANDLE heap = HeapCreate(0, 0, 0);
    void* blocks[256]; ok = heap != NULL;
    for(unsigned i = 0; ok && i < 256; i++) { blocks[i] = HeapAlloc(heap, HEAP_ZERO_MEMORY, 4096 + i * 4096); ok = blocks[i] && *(unsigned char*)blocks[i] == 0; if(ok) memset(blocks[i], 1, 4096 + i * 4096); }
    for(unsigned i = 0; ok && i < 256; i += 2) ok = HeapFree(heap, 0, blocks[i]);
    check(ok && HeapDestroy(heap), "a private heap allocates 130 MiB in 256 blocks and frees");

    print(failures ? "WoWPS5 Win32 memory test FAIL\r\n" : "WoWPS5 Win32 memory test PASS\r\n");
    ExitProcess(failures);
}
