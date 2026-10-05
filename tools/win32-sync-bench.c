/* What synchronisation and a server call cost under a given Wine. MIT license.
 * Each line is nanoseconds per operation, the median of five rounds. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static double now(void) { LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f); return (double)t.QuadPart * 1e9 / (double)f.QuadPart; }
static int order(const void* a, const void* b) { return *(const double*)a > *(const double*)b ? 1 : -1; }
#define MEASURE(name, count, body) do { double rounds[5]; for (int r = 0; r < 5; r++) { const double start = now(); \
    for (int i = 0; i < (count); i++) { body; } rounds[r] = (now() - start) / (count); } \
    qsort(rounds, 5, sizeof(double), order); printf("%-58s %9.0f ns\n", name, rounds[2]); fflush(stdout); } while (0)

static HANDLE ping, pong; static SRWLOCK lock = SRWLOCK_INIT; static CONDITION_VARIABLE turn = CONDITION_VARIABLE_INIT;
static volatile LONG stop, token;
static volatile LONG churning;
static void churn_once(void) { HANDLE e = CreateEventW(NULL, FALSE, FALSE, NULL); SetEvent(e); WaitForSingleObject(e, INFINITE); CloseHandle(e); }
static DWORD WINAPI churn(void* unused) { (void)unused; while (churning) churn_once(); return 0; }
static DWORD WINAPI echo(void* unused) { (void)unused; while (!stop) { WaitForSingleObject(ping, INFINITE); SetEvent(pong); } return 0; }
static DWORD WINAPI echo_cv(void* unused) { (void)unused; AcquireSRWLockExclusive(&lock);
    while (!stop) { while (!token && !stop) SleepConditionVariableSRW(&turn, &lock, INFINITE, 0); token = 0; WakeAllConditionVariable(&turn); }
    ReleaseSRWLockExclusive(&lock); return 0; }

int main(void) {
    HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL), semaphore = CreateSemaphoreW(NULL, 0, 1000, NULL), mutex = CreateMutexW(NULL, FALSE, NULL);
    CRITICAL_SECTION section; InitializeCriticalSection(&section); DWORD flags; HANDLE copy;
    { HANDLE never = CreateEventW(NULL, TRUE, FALSE, NULL);
      { SYSTEM_INFO info; GetSystemInfo(&info); DWORD_PTR process = 0, system = 0; GetProcessAffinityMask(GetCurrentProcess(), &process, &system);
      printf("processors: %lu, active %lu, process mask %llx, system mask %llx, thread priority %d, class %lx\n", (unsigned long)info.dwNumberOfProcessors,
             (unsigned long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), (unsigned long long)process, (unsigned long long)system,
             GetThreadPriority(GetCurrentThread()), (unsigned long)GetPriorityClass(GetCurrentProcess())); fflush(stdout); }
    MEASURE("Sleep(1)", 100, Sleep(1));
      MEASURE("Sleep(0)", 20000, Sleep(0));
      MEASURE("SwitchToThread", 20000, SwitchToThread());
      MEASURE("WaitForSingleObject(event, 1) that times out", 100, WaitForSingleObject(never, 1));
      MEASURE("Sleep(5)", 40, Sleep(5));
      CloseHandle(never); }
    LARGE_INTEGER tick; MEASURE("QueryPerformanceCounter", 500000, QueryPerformanceCounter(&tick));
    MEASURE("GetTickCount64", 2000000, (void)GetTickCount64());
    { char path[MAX_PATH], name[MAX_PATH]; GetTempPathA(MAX_PATH, path); GetTempFileNameA(path, "app", 0, name);
      HANDLE log = CreateFileA(name, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL); DWORD put;
      MEASURE("WriteFile 80 bytes, appended (a log line)", 2000, WriteFile(log, "0123456789012345678901234567890123456789012345678901234567890123456789012345678\n", 80, &put, NULL));
      CloseHandle(log); }
    MEASURE("critical section, uncontended enter+leave", 2000000, (EnterCriticalSection(&section), LeaveCriticalSection(&section)));
    MEASURE("SRW lock, uncontended acquire+release", 2000000, (AcquireSRWLockExclusive(&lock), ReleaseSRWLockExclusive(&lock)));
    MEASURE("event: SetEvent + WaitForSingleObject (signalled)", 100000, (SetEvent(event), WaitForSingleObject(event, INFINITE)));
    MEASURE("semaphore: Release + Wait (signalled)", 100000, (ReleaseSemaphore(semaphore, 1, NULL), WaitForSingleObject(semaphore, INFINITE)));
    MEASURE("mutex: Wait + Release, uncontended", 100000, (WaitForSingleObject(mutex, INFINITE), ReleaseMutex(mutex)));
    MEASURE("WaitForSingleObject(event, 0) that times out", 100000, WaitForSingleObject(event, 0));
    MEASURE("a plain server call: GetHandleInformation", 100000, GetHandleInformation(event, &flags));
    MEASURE("DuplicateHandle + CloseHandle", 50000, (DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS), CloseHandle(copy)));
    MEASURE("event per request: create, set, wait, close", 50000, churn_once());
    { HANDLE others[2]; churning = 1; for (int i = 0; i < 2; i++) others[i] = CreateThread(NULL, 0, churn, NULL, 0, NULL);
      MEASURE("the same while two other threads do it too", 50000, churn_once());
      churning = 0; WaitForMultipleObjects(2, others, TRUE, INFINITE); }
    ping = CreateEventW(NULL, FALSE, FALSE, NULL); pong = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE thread = CreateThread(NULL, 0, echo, NULL, 0, NULL);
    MEASURE("two threads, event ping-pong (one round trip)", 50000, (SetEvent(ping), WaitForSingleObject(pong, INFINITE)));
    stop = 1; SetEvent(ping); WaitForSingleObject(thread, 5000); stop = 0;
    thread = CreateThread(NULL, 0, echo_cv, NULL, 0, NULL);
    MEASURE("two threads, condition variable ping-pong", 50000, (AcquireSRWLockExclusive(&lock), token = 1, WakeAllConditionVariable(&turn),
        ({ while (token) SleepConditionVariableSRW(&turn, &lock, INFINITE, 0); }), ReleaseSRWLockExclusive(&lock)));
    AcquireSRWLockExclusive(&lock); stop = 1; WakeAllConditionVariable(&turn); ReleaseSRWLockExclusive(&lock); WaitForSingleObject(thread, 5000);
    static char block[65536]; char path[MAX_PATH], name[MAX_PATH]; GetTempPathA(MAX_PATH, path); GetTempFileNameA(path, "syn", 0, name);
    HANDLE file = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL); DWORD done;
    MEASURE("WriteFile 64 KiB at offset 0", 2000, (SetFilePointer(file, 0, NULL, FILE_BEGIN), WriteFile(file, block, sizeof(block), &done, NULL)));
    MEASURE("ReadFile 64 KiB at offset 0", 2000, (SetFilePointer(file, 0, NULL, FILE_BEGIN), ReadFile(file, block, sizeof(block), &done, NULL)));
    MEASURE("VirtualAlloc + touch + VirtualFree, 64 KiB", 20000, ({ char* p = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); p[0] = 1; VirtualFree(p, 0, MEM_RELEASE); }));
    MEASURE("VirtualProtect one page, twice", 50000, ({ DWORD old; VirtualProtect(block, 4096, PAGE_READONLY, &old); VirtualProtect(block, 4096, PAGE_READWRITE, &old); }));
    printf("sync bench done\n"); return 0;
}
