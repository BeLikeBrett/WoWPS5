/* The C runtime and a few common system DLLs through Wine. MIT license.
 * An ordinary MinGW program (CRT startup, main, stdio), unlike the CRT-free
 * tests: it needs Wine's msvcrt or ucrtbase, advapi32 and what they load.
 * Each check prints PASS or FAIL; the exit status is the number of failures. */
#include <windows.h>
#include <process.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;
static void check(int passed, const char* what) {
    if(!passed) failures++;
    printf("%s: %s\n", passed ? "PASS" : "FAIL", what);
    fflush(stdout);
}
static int compare(const void* a, const void* b) { return *(const int*)a - *(const int*)b; }
static unsigned __stdcall worker(void* argument) {
    double sum = 0;
    for(int i = 1; i <= 100000; i++) sum += sqrt((double)i);
    *(double*)argument = sum;
    return 7;
}
static jmp_buf escape;
static void jump(int value) { longjmp(escape, value); }

int main(int argc, char** argv) {
    (void)argv;
    check(argc >= 1, "main receives its arguments");

    char text[64];
    snprintf(text, sizeof(text), "%.3f %d %s", 3.14159, 42, "wow");
    check(!strcmp(text, "3.142 42 wow") && strtod("0.625", NULL) == 0.625 && atoi("1234") == 1234, "number formatting and parsing, '.' as the decimal point");

    int* numbers = malloc(1000 * sizeof(int));
    for(int i = 0; numbers && i < 1000; i++) numbers[i] = (i * 7919) % 1000;
    if(numbers) qsort(numbers, 1000, sizeof(int), compare);
    int sorted = numbers != NULL;
    for(int i = 1; sorted && i < 1000; i++) sorted = numbers[i - 1] <= numbers[i];
    numbers = realloc(numbers, 4000000 * sizeof(int));
    check(sorted && numbers && numbers[999] == 999, "malloc, qsort, realloc to 16 MiB");
    free(numbers);

    char path[MAX_PATH];
    const char* temp = getenv("TEMP");
    snprintf(path, sizeof(path), "%s\\wowps5-crt-test.txt", temp ? temp : ".");
    FILE* file = fopen(path, "w+b");
    int ok = file != NULL;
    if(ok) {
        for(int i = 0; i < 1000; i++) fprintf(file, "line %04d\n", i);
        ok = ftell(file) == 10000 && !fseek(file, 5000, SEEK_SET) && fgets(text, sizeof(text), file) && !strcmp(text, "line 0500\n");
        fclose(file);
    }
    check(temp && ok && !remove(path), "stdio file: write 1000 lines, seek, read one back, remove");

    const time_t now = time(NULL);
    struct tm* parts = localtime(&now);
    check(now > 1700000000 && parts && parts->tm_year >= 124, "the clock reads a date after 2023");

    LARGE_INTEGER frequency, before, after;
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&before);
    Sleep(50);
    QueryPerformanceCounter(&after);
    const double slept = (double)(after.QuadPart - before.QuadPart) * 1000.0 / (double)frequency.QuadPart;
    printf("info Sleep(50) took %.1f ms\n", slept);
    check(slept >= 45 && slept < 200, "Sleep(50) by the performance counter");

    double sum = 0; unsigned id = 0; DWORD status = 0;
    HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, worker, &sum, 0, &id);
    ok = thread && WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0 && GetExitCodeThread(thread, &status);
    check(ok && status == 7 && sum > 21082000 && sum < 21082020, "a CRT thread computes with the floating-point unit");
    if(thread) CloseHandle(thread);

    const int jumped = setjmp(escape);
    if(!jumped) jump(5);
    check(jumped == 5, "setjmp and longjmp");

    HKEY key; DWORD type = 0, bytes = sizeof(text);
    ok = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &key) == ERROR_SUCCESS;
    if(ok) { ok = RegQueryValueExA(key, "CurrentBuild", NULL, &type, (BYTE*)text, &bytes) == ERROR_SUCCESS && type == REG_SZ; RegCloseKey(key); }
    if(ok) printf("info Windows build %s\n", text);
    check(ok, "advapi32 reads the Windows version from the registry");

    char user[64], directory[MAX_PATH]; bytes = sizeof(user);
    ok = GetUserNameA(user, &bytes) && GetWindowsDirectoryA(directory, sizeof(directory)) && GetFileAttributesA(directory) != INVALID_FILE_ATTRIBUTES;
    if(ok) printf("info user %s, Windows directory %s\n", user, directory);
    check(ok, "the user name and the Windows directory");

    SYSTEM_INFO system; GetSystemInfo(&system);
    MEMORYSTATUSEX memory = { .dwLength = sizeof(memory) };
    ok = GlobalMemoryStatusEx(&memory);
    printf("info %lu processors, %llu MiB physical memory, %llu GiB address space\n", system.dwNumberOfProcessors,
           memory.ullTotalPhys >> 20, memory.ullTotalVirtual >> 30);
    check(ok && system.dwNumberOfProcessors >= 1 && memory.ullTotalPhys > (1ull << 30), "processor count and memory size");

    static SYSTEM_LOGICAL_PROCESSOR_INFORMATION topology[256]; DWORD topologyBytes = sizeof(topology); int cores = 0;
    ok = GetLogicalProcessorInformation(topology, &topologyBytes);
    for(DWORD i = 0; ok && i < topologyBytes / sizeof(*topology); i++) cores += topology[i].Relationship == RelationProcessorCore;
    printf("info %d processor cores in the topology\n", cores);
    check(ok && cores >= 1 && cores <= (int)system.dwNumberOfProcessors, "the processor topology lists the cores");

    ULARGE_INTEGER available, total;
    ok = GetDiskFreeSpaceExA("C:\\", &available, &total, NULL);
    if(ok) printf("info drive C: %llu GiB free of %llu\n", available.QuadPart >> 30, total.QuadPart >> 30);
    check(ok && GetDriveTypeA("C:\\") == DRIVE_FIXED, "drive C: is a fixed disk with a size");

    WIN32_FIND_DATAA found; int entries = 0, system32 = 0;
    HANDLE listing = FindFirstFileA("C:\\windows\\*", &found);
    if(listing != INVALID_HANDLE_VALUE) {
        do { entries++; system32 |= !lstrcmpiA(found.cFileName, "SYSTEM32") && (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY); } while(FindNextFileA(listing, &found));
        FindClose(listing);
    }
    printf("info C:\\windows lists %d entries\n", entries);
    check(entries > 3 && system32 && GetFileAttributesA("c:\\WINDOWS\\System32") != INVALID_FILE_ATTRIBUTES,
          "a directory listing finds system32, and a path in another case resolves");

    HMODULE library = LoadLibraryA("shlwapi.dll");
    typedef BOOL (WINAPI *MatchFunction)(const char*, const char*);
    MatchFunction match = library ? (MatchFunction)GetProcAddress(library, "PathMatchSpecA") : NULL;
    check(match && match("World.exe", "*.exe") && FreeLibrary(library), "LoadLibrary, GetProcAddress, a call into the loaded DLL");

    printf(failures ? "WoWPS5 Win32 crt test FAIL\n" : "WoWPS5 Win32 crt test PASS\n");
    return failures;
}
