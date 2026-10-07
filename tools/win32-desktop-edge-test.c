/* Corner cases of the desktop of a single-process Wine. MIT license.
 *
 * One source, two images: built with -DEDGE_DLL it is win32-desktop-edge-dll.dll,
 * which the program imports, so the DLL's DllMain runs under the loader lock
 * before main. The environment variable WOWPS5_EDGE selects the case:
 *
 *   (unset)     the first use of the desktop is on a worker thread that then
 *               exits; two threads create windows at once; a message sent to
 *               the desktop window returns; the shell window is the desktop.
 *   dllmain     the DLL creates a window in DllMain: no thread can be started
 *               there, so the desktop has to come from the calling thread.
 *   exitthread  the main thread ends with ExitThread while it is the last
 *               thread of the program: the process has to end by itself.
 *               Run it under a timeout; ending at all is the check.
 *
 * Every check prints PASS or FAIL; the exit status is the number of failures. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#ifdef EDGE_DLL

static HWND dll_window;
static DWORD dll_window_error, dll_desktop_tid;

__declspec(dllexport) HWND edge_dll_window(DWORD* error, DWORD* desktop_tid) {
    *error = dll_window_error;
    *desktop_tid = dll_desktop_tid;
    return dll_window;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void* reserved) {
    char mode[32];
    if(reason != DLL_PROCESS_ATTACH) return TRUE;
    if(!GetEnvironmentVariableA("WOWPS5_EDGE", mode, sizeof mode) || strcmp(mode, "dllmain")) return TRUE;
    /* a window made under the loader lock */
    SetLastError(0);
    dll_window = CreateWindowExA(0, "static", "made in DllMain", WS_POPUP, 0, 0, 64, 64, NULL, NULL, instance, NULL);
    dll_window_error = GetLastError();
    dll_desktop_tid = GetWindowThreadProcessId(GetDesktopWindow(), NULL);
    return TRUE;
}

#else

__declspec(dllimport) HWND edge_dll_window(DWORD* error, DWORD* desktop_tid);

static int failures;

static void check(int passed, const char* what) {
    if(!passed) failures++;
    printf("%s: %s", passed ? "PASS" : "FAIL", what);
    if(!passed) printf(" (GetLastError=%lu)", GetLastError());
    printf("\n");
    fflush(stdout);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if(message == WM_PAINT) { ValidateRect(window, NULL); return 0; }
    return DefWindowProcA(window, message, wparam, lparam);
}

static void pump(DWORD ms) {
    DWORD start = GetTickCount();
    MSG message;
    do {
        while(PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&message);
        Sleep(10);
    } while(GetTickCount() - start < ms);
}

/* the first thing in the process that needs the desktop, on a thread that never pumps messages */
static DWORD WINAPI first_user(void* argument) {
    DWORD* result = argument;
    HDC dc = GetDC(NULL);
    result[0] = dc != NULL;
    result[1] = GetDeviceCaps(dc, HORZRES);
    result[2] = GetWindowThreadProcessId(GetDesktopWindow(), NULL);
    ReleaseDC(NULL, dc);
    return 0;
}

static HANDLE go;

static DWORD WINAPI window_maker(void* argument) {
    HWND window;
    WaitForSingleObject(go, 5000);
    window = CreateWindowExA(0, "WoWPS5EdgeTest", "edge", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 50, 50, 200, 100, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if(!window) return GetLastError() ? GetLastError() : 1;
    pump(300);
    DestroyWindow(window);
    return 0;
}

int main(void) {
    WNDCLASSEXA window_class = {sizeof window_class};
    char mode[32] = "";
    DWORD first[3] = {0}, code[2] = {99, 99}, desktop_tid, dll_error = 0, dll_desktop_tid = 0, start;
    DWORD_PTR reply = 0;
    HANDLE threads[2];
    HWND desktop, window, dll_window;

    GetEnvironmentVariableA("WOWPS5_EDGE", mode, sizeof mode);
    printf("WoWPS5 desktop edge test: mode \"%s\", main thread %lu\n", mode, GetCurrentThreadId());
    fflush(stdout);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleA(NULL);
    window_class.lpszClassName = "WoWPS5EdgeTest";
    check(RegisterClassExA(&window_class) != 0, "RegisterClassEx");
    dll_window = edge_dll_window(&dll_error, &dll_desktop_tid);

    if(!strcmp(mode, "dllmain")) {
        printf("INFO: DllMain made window %p (error %lu); the desktop window belonged to thread %lu then\n",
               (void*)dll_window, dll_error, dll_desktop_tid);
        check(dll_window != NULL, "CreateWindowEx inside DllMain, under the loader lock");
        desktop_tid = GetWindowThreadProcessId(GetDesktopWindow(), NULL);
        printf("INFO: the desktop window belongs to thread %lu now (%s)\n", desktop_tid,
               desktop_tid == GetCurrentThreadId() ? "the main thread: no dedicated desktop thread" : "not the main thread");
        check(desktop_tid != 0, "the desktop window has an owner thread");
        window = CreateWindowExA(0, "WoWPS5EdgeTest", "edge", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 50, 50, 200, 100, NULL, NULL, window_class.hInstance, NULL);
        check(window != NULL, "CreateWindowEx after DllMain");
        pump(300);
        check(SendMessageTimeoutA(GetDesktopWindow(), WM_NULL, 0, 0, SMTO_NORMAL, 2000, &reply) != 0, "a message sent to the desktop window returns");
        if(window) DestroyWindow(window);
        if(dll_window) DestroyWindow(dll_window);
    }
    else if(!strcmp(mode, "exitthread")) {
        window = CreateWindowExA(0, "WoWPS5EdgeTest", "edge", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 50, 50, 200, 100, NULL, NULL, window_class.hInstance, NULL);
        check(window != NULL, "CreateWindowEx");
        pump(300);
        if(window) DestroyWindow(window);
        printf("the main thread calls ExitThread(%d) now; the process has to end by itself\n", failures);
        fflush(stdout);
        ExitThread(failures);
    }
    else {
        threads[0] = CreateThread(NULL, 0, first_user, first, 0, NULL);
        check(WaitForSingleObject(threads[0], 40000) == WAIT_OBJECT_0, "a worker thread's GetDC(NULL) returns");
        CloseHandle(threads[0]);
        printf("INFO: worker saw a %lu pixel wide screen; the desktop window belonged to thread %lu\n", first[1], first[2]);
        check(first[0] && first[1] > 0, "GetDC(NULL) on a worker thread gave a screen DC");

        desktop = GetDesktopWindow();
        desktop_tid = GetWindowThreadProcessId(desktop, NULL);
        printf("INFO: desktop window %p, owner thread %lu, after the worker thread exited\n", (void*)desktop, desktop_tid);
        check(desktop_tid != 0 && desktop_tid != GetCurrentThreadId(), "the desktop window survived its first user and has its own thread");
        start = GetTickCount();
        check(SendMessageTimeoutA(desktop, WM_NULL, 0, 0, SMTO_NORMAL, 2000, &reply) != 0, "a message sent to the desktop window returns");
        printf("INFO: the send took %lu ms\n", GetTickCount() - start);
        check(GetShellWindow() == desktop, "GetShellWindow is the desktop window");
        check(IsWindowVisible(desktop), "the desktop window is visible");

        go = CreateEventA(NULL, TRUE, FALSE, NULL);
        threads[0] = CreateThread(NULL, 0, window_maker, NULL, 0, NULL);
        threads[1] = CreateThread(NULL, 0, window_maker, NULL, 0, NULL);
        SetEvent(go);
        check(WaitForMultipleObjects(2, threads, TRUE, 20000) == WAIT_OBJECT_0, "two threads creating windows at once finish");
        GetExitCodeThread(threads[0], &code[0]);
        GetExitCodeThread(threads[1], &code[1]);
        check(code[0] == 0 && code[1] == 0, "both windows were created");

        check(ChangeDisplaySettingsExA(NULL, NULL, NULL, 0, NULL) == DISP_CHANGE_SUCCESSFUL, "ChangeDisplaySettingsEx(NULL) restores the registry mode");
    }

    printf("WoWPS5 desktop edge test %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    fflush(stdout);
    return failures;
}

#endif
