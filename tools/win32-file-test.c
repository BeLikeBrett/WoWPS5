/* File operations a game's data container relies on. MIT license.
 * Locks, overlapped and completion-port I/O, shared writable mappings,
 * resizing, renaming over a file, directory listing. Each check reports
 * the error it left and how long it took. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int failures;
static double now(void) { LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f); return (double)t.QuadPart * 1e3 / (double)f.QuadPart; }
static double started;
static void begin(void) { SetLastError(0); started = now(); }
static void check(BOOL passed, const char* what) {
    const DWORD error = GetLastError();
    if (!passed) failures++;
    printf("%s: %s (error %lu, %.1f ms)\n", passed ? "PASS" : "FAIL", what, passed ? 0 : error, now() - started); fflush(stdout);
}

int main(int argc, char** argv) {
    char directory[MAX_PATH], path[MAX_PATH], other[MAX_PATH];
    if (argc > 1) strcpy(directory, argv[1]); else GetTempPathA(MAX_PATH, directory);
    snprintf(path, sizeof(path), "%s\\wowps5-file-test.bin", directory); snprintf(other, sizeof(other), "%s\\wowps5-file-test.new", directory);
    printf("in %s\n", directory);
    static char block[1 << 16]; memset(block, 0x5a, sizeof(block)); DWORD done = 0; OVERLAPPED at;

    begin(); HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS, 0, NULL);
    check(file != INVALID_HANDLE_VALUE, "create a file for reading and writing, shared");
    begin(); BOOL ok = TRUE; for (int i = 0; ok && i < 16; i++) ok = WriteFile(file, block, sizeof(block), &done, NULL) && done == sizeof(block);
    check(ok, "write 1 MiB in 64 KiB pieces");
    begin(); check(FlushFileBuffers(file), "FlushFileBuffers");
    LARGE_INTEGER size; begin(); check(GetFileSizeEx(file, &size) && size.QuadPart == 1 << 20, "the size is 1 MiB");

    begin(); memset(&at, 0, sizeof(at)); at.Offset = 0x1000;
    check(LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 0x1000, 0, &at), "LockFileEx, exclusive, 4 KiB at 4 KiB");
    HANDLE second = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    begin(); memset(&at, 0, sizeof(at)); at.Offset = 0x1000;
    ok = second != INVALID_HANDLE_VALUE && !LockFileEx(second, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 0x1000, 0, &at) && GetLastError() == ERROR_LOCK_VIOLATION;
    check(ok, "a second handle is refused the same range (ERROR_LOCK_VIOLATION)");
    begin(); memset(&at, 0, sizeof(at)); at.Offset = 0x8000;
    check(second != INVALID_HANDLE_VALUE && LockFileEx(second, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 0x1000, 0, &at) && UnlockFileEx(second, 0, 0x1000, 0, &at),
          "and is given another range, and unlocks it");
    begin(); memset(&at, 0, sizeof(at)); at.Offset = 0x1000; check(UnlockFileEx(file, 0, 0x1000, 0, &at), "UnlockFileEx");
    begin(); check(LockFile(file, 0, 0, 1, 0) && UnlockFile(file, 0, 0, 1, 0), "LockFile and UnlockFile of the first byte");
    begin(); memset(&at, 0, sizeof(at)); check(LockFileEx(file, 0, 0, 0x100, 0, &at) && UnlockFileEx(file, 0, 0x100, 0, &at), "a shared lock, waiting allowed");
    if (second != INVALID_HANDLE_VALUE) CloseHandle(second);

    begin(); HANDLE mapping = CreateFileMappingA(file, NULL, PAGE_READWRITE, 0, 0, NULL);
    unsigned char* view = mapping ? MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0) : NULL;
    check(view && view[0] == 0x5a && view[(1 << 20) - 1] == 0x5a, "a writable view of the whole file");
    if (view) { view[0x12345] = 0x77; view[0xfffff] = 0x78; }
    begin(); check(view && FlushViewOfFile(view, 0), "FlushViewOfFile");
    unsigned char byte = 0; begin(); SetFilePointer(file, 0x12345, NULL, FILE_BEGIN);
    check(ReadFile(file, &byte, 1, &done, NULL) && byte == 0x77, "ReadFile sees what was written through the view");
    begin(); SetFilePointer(file, 0x20000, NULL, FILE_BEGIN); byte = 0x33;
    check(WriteFile(file, &byte, 1, &done, NULL) && view && view[0x20000] == 0x33, "the view sees what WriteFile wrote");
    unsigned char* reader = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0x10000, 0x10000) : NULL;
    begin(); check(reader && reader[0x2345] == 0x77, "a second, read-only view at an offset sees it too");
    if (reader) UnmapViewOfFile(reader);
    if (view) UnmapViewOfFile(view);
    if (mapping) CloseHandle(mapping);

    begin(); size.QuadPart = 3 << 20; check(SetFilePointerEx(file, size, NULL, FILE_BEGIN) && SetEndOfFile(file) && GetFileSizeEx(file, &size) && size.QuadPart == 3 << 20, "grow to 3 MiB with SetEndOfFile");
    begin(); size.QuadPart = 1 << 19; check(SetFilePointerEx(file, size, NULL, FILE_BEGIN) && SetEndOfFile(file) && GetFileSizeEx(file, &size) && size.QuadPart == 1 << 19, "shrink to 512 KiB");
    CloseHandle(file);

    begin(); HANDLE async = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    check(async != INVALID_HANDLE_VALUE, "open for overlapped I/O");
    memset(&at, 0, sizeof(at)); at.Offset = 0x12345; at.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL); byte = 0; done = 0;
    begin(); ok = ReadFile(async, &byte, 1, NULL, &at) || GetLastError() == ERROR_IO_PENDING;
    ok = ok && GetOverlappedResult(async, &at, &done, TRUE) && done == 1 && byte == 0x77;
    check(ok, "an overlapped read completes through its event");
    memset(block, 0x11, sizeof(block)); at.Offset = 0x40000; ResetEvent(at.hEvent);
    begin(); ok = WriteFile(async, block, sizeof(block), NULL, &at) || GetLastError() == ERROR_IO_PENDING;
    check(ok && GetOverlappedResult(async, &at, &done, TRUE) && done == sizeof(block), "an overlapped write of 64 KiB completes");
    HANDLE port = CreateIoCompletionPort(async, NULL, 0x1234, 0);
    OVERLAPPED queued; memset(&queued, 0, sizeof(queued)); queued.Offset = 0x40000; byte = 0;
    begin(); ok = port && (ReadFile(async, &byte, 1, NULL, &queued) || GetLastError() == ERROR_IO_PENDING);
    ULONG_PTR key = 0; OVERLAPPED* finished = NULL; done = 0;
    ok = ok && GetQueuedCompletionStatus(port, &done, &key, &finished, 5000) && key == 0x1234 && finished == &queued && done == 1 && byte == 0x11;
    check(ok, "a read reports to a completion port");
    begin(); ok = port && PostQueuedCompletionStatus(port, 7, 9, NULL) && GetQueuedCompletionStatus(port, &done, &key, &finished, 5000) && done == 7 && key == 9;
    check(ok, "a posted completion is received");
    if (port) CloseHandle(port);
    CloseHandle(at.hEvent); CloseHandle(async);

    begin(); HANDLE fresh = CreateFileA(other, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    ok = fresh != INVALID_HANDLE_VALUE && WriteFile(fresh, "new", 3, &done, NULL); if (fresh != INVALID_HANDLE_VALUE) CloseHandle(fresh);
    check(ok && MoveFileExA(other, path, MOVEFILE_REPLACE_EXISTING), "MoveFileEx over an existing file");
    begin(); WIN32_FILE_ATTRIBUTE_DATA attributes; check(GetFileAttributesExA(path, GetFileExInfoStandard, &attributes) && attributes.nFileSizeLow == 3, "and the name now has the new contents");

    char pattern[MAX_PATH]; snprintf(pattern, sizeof(pattern), "%s\\*", directory); WIN32_FIND_DATAA found; int entries = 0, seen = 0;
    begin(); HANDLE search = FindFirstFileA(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) { do { entries++; if (!strcmp(found.cFileName, "wowps5-file-test.bin")) seen = 1; } while (FindNextFileA(search, &found)); FindClose(search); }
    printf("     %d directory entries\n", entries);
    check(seen, "the directory listing has the file");
    begin(); HANDLE held = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    ok = held != INVALID_HANDLE_VALUE && DeleteFileA(path); if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    check(ok && GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES, "delete while open, gone after the close");
    ULARGE_INTEGER available; begin(); check(GetDiskFreeSpaceExA(directory, &available, NULL, NULL), "GetDiskFreeSpaceEx");
    printf(failures ? "WoWPS5 file test FAIL (%d)\n" : "WoWPS5 file test PASS\n", failures);
    return failures;
}
