// runtime/wine-ps5/ps5_mman.c against the console's own kernel. MIT license.
// The calls Wine's virtual.c makes, at the addresses the client needs, with
// the direct pool read before and after: what the host model cannot establish.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#ifndef __PROSPERO__
bool wowps5MmanConsoleTest(const char* reportPath) { (void)reportPath; return true; }
#else
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <ps5platform/memory.h>
#include "../../runtime/wine-ps5/ps5_mman.h"

static FILE* report;
static unsigned failures;
static void say(const char* format, ...) {
    char line[600]; va_list arguments;
    va_start(arguments, format); vsnprintf(line, sizeof(line), format, arguments); va_end(arguments);
    fprintf(stderr, "[WoWPS5 Wine] %s\n", line); fflush(stderr);
    if(report) { fprintf(report, "%s\n", line); fflush(report); }
}
static bool verdict(bool passed, const char* what) {
    if(!passed) failures++;
    say("%s: %s", passed ? "PASS" : "FAIL", what);
    return passed;
}
static double milliseconds(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec*1000.0 + now.tv_nsec/1e6;
}
static uint64_t directFree(void) {
    struct ps5_memory_stats stats;
    return ps5_memory_query(&stats) ? stats.free_bytes : 0;
}
#define ANON (MAP_PRIVATE|MAP_ANON)
#define RW (PROT_READ|PROT_WRITE)

bool wowps5MmanConsoleTest(const char* reportPath) {
    report = fopen(reportPath, "w"); failures = 0;
    ps5w_init(NULL);
    struct ps5w_stats stats;
    const uint64_t baseline = directFree();
    say("mman begins direct_free_mib=%llu", (unsigned long long)(baseline>>20));

    // Wine's reserved areas: the low 4 MiB below the title, the page Windows reads the clock from, and its main area
    uint8_t* const low = (uint8_t*)0x10000, * const shared = (uint8_t*)0x7ffe0000, * const area = (uint8_t*)0x1000000000ull;
    const size_t areaBytes = (size_t)8<<30;
    double t = milliseconds();
    const bool reserved = ps5w_mmap(low, 0x3f0000, PROT_NONE, ANON|MAP_FIXED|MAP_EXCL, -1, 0) == low &&
        ps5w_mmap(shared, 0x10000, PROT_NONE, ANON|MAP_FIXED|MAP_EXCL, -1, 0) == shared &&
        ps5w_mmap(area, areaBytes, PROT_NONE, ANON|MAP_FIXED|MAP_EXCL, -1, 0) == area;
    say("mman reserve low_4mib kuser 8gib ms=%.2f errno=%d", milliseconds()-t, reserved ? 0 : errno);
    if(!verdict(reserved, "exclusive reservations at Wine's addresses")) goto done;
    errno = 0;
    verdict(ps5w_mmap((void*)0x400000, 0x10000, PROT_NONE, ANON|MAP_FIXED|MAP_EXCL, -1, 0) == MAP_FAILED,
            "an exclusive mapping over the title's own code is refused");
    say("mman exclusive_over_title errno=%d", errno);

    // KUSER_SHARED_DATA: one page, committed read-write, then read-only
    bool ok = !ps5w_mprotect(shared, 0x1000, RW);
    if(ok) { memcpy(shared+0x26c, "\x0a\0\0\0", 4); ok = !ps5w_mprotect(shared, 0x1000, PROT_READ) && shared[0x26c] == 10; }
    verdict(ok, "the shared-data page at 0x7ffe0000 commits, takes data and turns read-only");

    // one page committed inside the area, the rest of its unit untouched
    const uint64_t before = directFree();
    ps5w_live(&stats);
    const uint64_t heldBefore = stats.direct_bytes;
    ok = !ps5w_mprotect(area+0x4000, 0x4000, RW);
    bool zero = ok;
    for(size_t i = 0; ok && i < 0x4000; i++) zero &= area[0x4000+i] == 0;
    ps5w_live(&stats);
    say("mman commit_one_page direct_bytes_delta=%llu direct_free_delta=%lld", (unsigned long long)(stats.direct_bytes-heldBefore), (long long)(before-directFree()));
    verdict(ok && zero && stats.direct_bytes-heldBefore == PS5W_UNIT && before-directFree() == PS5W_UNIT, "one page costs one 64 KiB unit, zeroed");
    if(ok) memset(area+0x4000, 0x77, 0x4000);
    ok = ok && !ps5w_mprotect(area+0xc000, 0x4000, RW) && area[0xc000] == 0 && area[0x7fff] == 0x77;
    ok = ok && ps5w_mmap(area+0x4000, 0x4000, PROT_NONE, ANON|MAP_FIXED, -1, 0) == area+0x4000 && before-directFree() == PS5W_UNIT;
    ok = ok && ps5w_mmap(area+0xc000, 0x4000, PROT_NONE, ANON|MAP_FIXED, -1, 0) == area+0xc000;
    verdict(ok && directFree() == before, "pages of one unit come and go separately, and the unit returns with the last");

    // code: written, then made read-execute, then rewritten in place as read-write-execute
    static const uint8_t code[] = { 0xb8, 0x2a, 0, 0, 0, 0xc3 };
    uint8_t* const text = area + 0x100000;
    int answer = -1, rewritten = -1;
    if(ps5w_mmap(text, 0x10000, RW, ANON|MAP_FIXED, -1, 0) == text) {
        memcpy(text, code, sizeof(code));
        if(!ps5w_mprotect(text, 0x10000, PROT_READ|PROT_EXEC)) answer = ((int(*)(void))text)();
        if(!ps5w_mprotect(text, 0x10000, RW|PROT_EXEC)) { text[1] = 0x2b; rewritten = ((int(*)(void))text)(); }
    }
    say("mman code read_execute=%d read_write_execute=%d", answer, rewritten);
    verdict(answer == 42 && rewritten == 43, "anonymous memory becomes executable through mprotect, as PE sections need");

    // WowB.exe's whole image at its preferred base, as one view
    uint8_t* const image = (uint8_t*)0x140000000ull; const size_t imageBytes = 0x8590000;
    t = milliseconds();
    ok = ps5w_mmap(image, imageBytes, RW, ANON|MAP_FIXED|MAP_EXCL, -1, 0) == image;
    const double mapped = milliseconds()-t;
    if(ok) { image[0] = 'M'; image[imageBytes-1] = 'Z'; ok = !ps5w_mprotect(image, 0x4000, PROT_READ) && !ps5w_mprotect(image+0x4000, 0x100000, PROT_READ|PROT_EXEC); }
    ps5w_live(&stats);
    say("mman image bytes=0x%zx ms=%.2f allocations=%llu errno=%d", imageBytes, mapped, (unsigned long long)stats.allocations, ok ? 0 : errno);
    verdict(ok, "a 134 MiB view at 0x140000000, with per-section protections");

    // a large commit, the middle of it released, then taken again
    uint8_t* const heap = area + 0x10000000; const size_t heapBytes = (size_t)1<<30, hole = 256u<<20;
    t = milliseconds();
    ok = !ps5w_mprotect(heap, heapBytes, RW);
    const double committed = milliseconds()-t;
    const uint64_t held = baseline - directFree();
    if(ok) for(size_t at = 0; at < heapBytes; at += 0x10000) heap[at] = (uint8_t)(at>>16);
    t = milliseconds();
    ok = ok && !ps5w_munmap(heap+hole, hole);
    const double released = milliseconds()-t;
    const uint64_t afterHole = baseline - directFree();
    ok = ok && heap[hole-0x10000] == (uint8_t)((hole-0x10000)>>16) && heap[2*hole] == (uint8_t)((2*hole)>>16);
    ok = ok && ps5w_mmap(heap+hole, hole, RW, ANON|MAP_FIXED, -1, 0) == heap+hole && heap[hole] == 0 && heap[2*hole-1] == 0;
    say("mman heap commit_1gib_ms=%.1f held_mib=%llu release_256mib_ms=%.1f held_after_mib=%llu errno=%d", committed,
        (unsigned long long)(held>>20), released, (unsigned long long)(afterHole>>20), ok ? 0 : errno);
    verdict(ok && held - afterHole == hole, "part of a large allocation is released and reused; the rest keeps its contents");

    // many separate units: every other unit of 512 MiB, each its own allocation and mapping
    uint8_t* const sparse = area + 0x80000000ull; unsigned made = 0;
    t = milliseconds();
    for(size_t at = 0; at < (512u<<20); at += 0x20000) {
        if(ps5w_mprotect(sparse+at, 0x10000, RW)) break;
        sparse[at] = 1; made++;
    }
    say("mman sparse units=%u ms=%.1f errno=%d", made, milliseconds()-t, made == 4096 ? 0 : errno);
    verdict(made == 4096, "4096 separate direct mappings in one process");

    // a file view placed inside the reserved area, and anonymous memory over part of it
    const int file = open("/data/wowps5/client/WowB.exe", O_RDONLY);
    uint8_t* const view = area + 0xc0000000ull;
    errno = 0;
    const bool refused = file >= 0 && ps5w_mmap(view, 0x10000, PROT_READ|PROT_EXEC, MAP_PRIVATE|MAP_FIXED, file, 0) == MAP_FAILED && errno == ENODEV;
    ok = file >= 0 && ps5w_mmap(view, 0x10000, PROT_READ, MAP_PRIVATE|MAP_FIXED, file, 0) == view && view[0] == 'M' && view[1] == 'Z';
    const int fileErrno = ok ? 0 : errno;
    ok = ok && !ps5w_mprotect(view, 0x10000, RW) && (view[0] = 'm') == 'm';
    ok = ok && ps5w_mmap(view, 0x4000, RW, ANON|MAP_FIXED, -1, 0) == view && view[0] == 0 && view[0x4000] != 0;
    say("mman file fd=%d exec_refused=%d errno=%d", file, refused, fileErrno);
    verdict(refused && ok, "a file view at a fixed address in the reserved area, copy-on-write, partly replaced");
    if(file >= 0) close(file);

    t = milliseconds();
    ok = !ps5w_munmap(area, areaBytes) && !ps5w_munmap(image, imageBytes) && !ps5w_munmap(shared, 0x10000) && !ps5w_munmap(low, 0x3f0000);
    ps5w_live(&stats);
    say("mman release_all ms=%.1f direct_bytes=%llu committed=%llu reserved=%llu file=%llu direct_free_delta=%lld allocations=%llu", milliseconds()-t,
        (unsigned long long)stats.direct_bytes, (unsigned long long)stats.committed_pages, (unsigned long long)stats.reserved_pages,
        (unsigned long long)stats.file_pages, (long long)(baseline-directFree()), (unsigned long long)stats.allocations);
    verdict(ok && !stats.direct_bytes && !stats.committed_pages && !stats.reserved_pages && !stats.file_pages && directFree() == baseline,
            "everything unmaps and the direct pool is back where it started");
    ok = ps5w_mmap(area, 0x10000, PROT_NONE, ANON|MAP_FIXED|MAP_EXCL, -1, 0) == area && !ps5w_munmap(area, 0x10000);
    verdict(ok, "the released range is free for an exclusive mapping again");
done:
    say("mman ends failures=%u", failures);
    if(report) { fclose(report); report = NULL; }
    return !failures;
}
#endif
