/* Step a of the single-process desktop work: a C-runtime console program.
 * MIT license.
 *
 * Uses what a MinGW program gets from the C runtime DLL (msvcrt or ucrtbase,
 * whichever the toolchain links): startup code and argv, printf, malloc/
 * realloc/free, fopen/fwrite/fread/fseek/remove, time/clock/localtime,
 * _beginthreadex with a C-runtime per-thread value (errno), qsort, strtod and
 * floating-point formatting, setlocale, getenv, atexit.
 * Every check prints PASS or FAIL; the exit status is the number of failures. */
#include <windows.h>
#include <errno.h>
#include <locale.h>
#include <math.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;
static volatile LONG thread_sum;
static int atexit_ran;

static void check(int passed, const char* what) {
    if(!passed) failures++;
    printf("%s: %s\n", passed ? "PASS" : "FAIL", what);
    fflush(stdout);
}

static int compare_int(const void* a, const void* b) {
    return *(const int*)a - *(const int*)b;
}

static unsigned __stdcall worker(void* argument) {
    int index = (int)(INT_PTR)argument;
    char* block = malloc(4096);
    errno = 1000 + index;                 /* per-thread in the C runtime */
    if(!block) return 1;
    memset(block, index, 4096);
    Sleep(20);
    if(errno != 1000 + index || block[4095] != index) { free(block); return 2; }
    free(block);
    InterlockedExchangeAdd(&thread_sum, index);
    return 0;
}

static void at_exit(void) {
    /* the last line of a passing run; a missing line means atexit did not run */
    printf("atexit handler ran (failures=%d)\n", failures);
    fflush(stdout);
    atexit_ran = 1;
}

int main(int argc, char** argv) {
    char path[MAX_PATH + 64], temp[MAX_PATH], line[128];
    unsigned char data[8192], back[8192];
    HANDLE threads[4];
    unsigned thread_id;
    FILE* file;
    int values[64], sorted = 1;
    void* big;
    time_t now;
    struct tm* local;
    clock_t ticks;
    double parsed;
    DWORD code, i;

    atexit(at_exit);
    printf("WoWPS5 C runtime test: argc=%d argv0=%s\n", argc, argv[0] ? argv[0] : "(null)");
    check(argc >= 1 && argv[0] && argv[0][0], "startup code delivered argv");

    /* formatting */
    snprintf(line, sizeof line, "%d %5.2f %s %x %lld", -42, 3.14159, "text", 0xbeefu, 1234567890123LL);
    check(!strcmp(line, "-42  3.14 text beef 1234567890123"), "snprintf integer, float, string, hex, 64-bit");
    parsed = strtod("2.5e3", NULL);
    check(parsed == 2500.0 && fabs(sqrt(parsed) - 50.0) < 1e-12, "strtod and sqrt");
    check(setlocale(LC_ALL, "C") != NULL, "setlocale");

    /* heap */
    big = malloc(64 * 1024 * 1024);
    check(big != NULL, "malloc 64 MiB");
    if(big) {
        memset(big, 0x5a, 64 * 1024 * 1024);
        big = realloc(big, 96 * 1024 * 1024);
        check(big && ((unsigned char*)big)[64 * 1024 * 1024 - 1] == 0x5a, "realloc to 96 MiB keeps contents");
        free(big);
    }
    for(i = 0; i < 64; i++) values[i] = (int)((i * 2654435761u) % 1000);
    qsort(values, 64, sizeof values[0], compare_int);
    for(i = 1; i < 64; i++) if(values[i - 1] > values[i]) sorted = 0;
    check(sorted, "qsort");

    /* stdio on a file */
    check(GetTempPathA(sizeof temp, temp) > 0, "GetTempPath");
    snprintf(path, sizeof path, "%swowps5-crt-test-%lu.bin", temp, GetCurrentProcessId());
    for(i = 0; i < sizeof data; i++) data[i] = (unsigned char)(i * 7 + 3);
    file = fopen(path, "wb");
    check(file != NULL, "fopen for writing");
    if(file) {
        check(fwrite(data, 1, sizeof data, file) == sizeof data, "fwrite 8192 bytes");
        check(fprintf(file, "tail %d\n", 77) == 8, "fprintf to file");
        check(fclose(file) == 0, "fclose");
    }
    file = fopen(path, "rb");
    check(file != NULL, "fopen for reading");
    if(file) {
        memset(back, 0, sizeof back);
        check(fread(back, 1, sizeof back, file) == sizeof back && !memcmp(data, back, sizeof data), "fread returns what was written");
        check(fgets(line, sizeof line, file) && !strcmp(line, "tail 77\n"), "fgets after the binary block");
        check(fseek(file, 0, SEEK_END) == 0 && ftell(file) == (long)sizeof data + 8, "fseek/ftell file size");
        fclose(file);
    }
    check(remove(path) == 0, "remove");
    check(fopen(path, "rb") == NULL && errno == ENOENT, "fopen of the removed file sets ENOENT");

    /* time */
    now = time(NULL);
    local = localtime(&now);
    check(now > 1700000000 && local && local->tm_year >= 125, "time and localtime are plausible");
    if(local && strftime(line, sizeof line, "%Y-%m-%d %H:%M:%S", local)) printf("local time %s\n", line);
    ticks = clock();
    Sleep(50);
    check(clock() - ticks >= 40 * CLOCKS_PER_SEC / 1000, "clock advances across Sleep(50)");

    /* threads through the C runtime */
    for(i = 0; i < 4; i++)
        threads[i] = (HANDLE)_beginthreadex(NULL, 0, worker, (void*)(INT_PTR)(i + 1), 0, &thread_id);
    check(threads[0] && threads[1] && threads[2] && threads[3], "_beginthreadex x4");
    check(WaitForMultipleObjects(4, threads, TRUE, 10000) == WAIT_OBJECT_0, "threads finish");
    for(i = 0, code = 0; i < 4; i++) {
        DWORD one = 99;
        GetExitCodeThread(threads[i], &one);
        code |= one;
        CloseHandle(threads[i]);
    }
    check(code == 0 && thread_sum == 10, "each thread kept its own errno and heap block");

    check(getenv("WINDIR") != NULL || getenv("windir") != NULL, "getenv sees the Windows environment");

    printf("WoWPS5 C runtime test %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    fflush(stdout);
    return failures;
}
