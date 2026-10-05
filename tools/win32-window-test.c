/* Step b of the single-process desktop work: a window, a message loop, GDI
 * painting, a timer and synthetic input. MIT license.
 *
 *   win32-window-test.exe [seconds]      (default 3)
 *
 * Every check prints PASS or FAIL; the exit status is the number of failures.
 * Lines starting with INFO are facts about the run, not checks. The first
 * failing line is the diagnostic: each step names the call and the Win32
 * error. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILL_COLOR RGB(32, 96, 200)
#define MARK_COLOR RGB(250, 220, 40)

static int failures;
static int creates, sizes, paints, timers, key_downs, chars, mouse_moves, destroys, erases, raw_inputs;
static int client_width, client_height;
static DWORD window_thread;

static void check(int passed, const char* what) {
    if(!passed) failures++;
    printf("%s: %s", passed ? "PASS" : "FAIL", what);
    if(!passed) printf(" (GetLastError=%lu)", GetLastError());
    printf("\n");
    fflush(stdout);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch(message) {
    case WM_CREATE:
        creates++;
        window_thread = GetCurrentThreadId();
        return 0;
    case WM_SIZE:
        sizes++;
        client_width = LOWORD(lparam);
        client_height = HIWORD(lparam);
        return 0;
    case WM_ERASEBKGND:
        erases++;
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        RECT client, mark = {40, 40, 120, 120};
        HDC dc = BeginPaint(window, &paint);
        HBRUSH fill = CreateSolidBrush(FILL_COLOR), yellow = CreateSolidBrush(MARK_COLOR);
        char text[64];
        GetClientRect(window, &client);
        FillRect(dc, &client, fill);
        FillRect(dc, &mark, yellow);
        SelectObject(dc, GetStockObject(WHITE_PEN));
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, 150, 40, 300, 120);
        MoveToEx(dc, 0, 0, NULL);
        LineTo(dc, client.right, client.bottom);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        snprintf(text, sizeof text, "WoWPS5 window test: paint %d, timer %d", paints + 1, timers);
        TextOutA(dc, 40, 140, text, (int)strlen(text));
        DeleteObject(fill);
        DeleteObject(yellow);
        EndPaint(window, &paint);
        paints++;
        return 0;
    }
    case WM_TIMER:
        if(wparam == 1) {
            timers++;
            if(timers % 5 == 0) InvalidateRect(window, NULL, FALSE);  /* repaint twice a second */
        }
        return 0;
    case WM_KEYDOWN:
        key_downs++;
        return 0;
    case WM_CHAR:
        chars++;
        return 0;
    case WM_MOUSEMOVE:
        mouse_moves++;
        return 0;
    case WM_INPUT:
        raw_inputs++;
        break;
    case WM_DESTROY:
        destroys++;
        PostQuitMessage(7);
        return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

/* Draw text into a memory bitmap and count the pixels that changed: font
 * selection and glyph rendering without any window. */
static int text_pixels(void) {
    BITMAPINFO info = {{sizeof(BITMAPINFOHEADER), 256, -64, 1, 32, BI_RGB}};
    DWORD* bits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    HFONT font = CreateFontA(32, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "Tahoma");
    int count = 0, i;
    if(!dc || !bitmap || !bits) return -1;
    SelectObject(dc, bitmap);
    SelectObject(dc, font);
    SetBkColor(dc, RGB(0, 0, 0));
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutA(dc, 4, 4, "WoWPS5", 6);
    GdiFlush();
    for(i = 0; i < 256 * 64; i++) if(bits[i] & 0xffffff) count++;
    DeleteObject(font);
    DeleteDC(dc);
    DeleteObject(bitmap);
    return count;
}

int main(int argc, char** argv) {
    DWORD run_ms = (argc > 1 ? (DWORD)atoi(argv[1]) : 3) * 1000, start, desktop_pid = 0, desktop_tid;
    WNDCLASSEXA window_class = {sizeof window_class};
    DISPLAY_DEVICEA device = {sizeof device};
    DEVMODEA mode = {.dmSize = sizeof mode};
    RECT rect = {0, 0, 640, 360}, window_rect, client_rect = {0};
    BOOL input_sent = FALSE, closed = FALSE;
    INPUT input[2];
    COLORREF pixel;
    HWND window, desktop;
    HDC dc;
    MSG message;
    int glyphs;

    printf("WoWPS5 window test: pid %lu thread %lu\n", GetCurrentProcessId(), GetCurrentThreadId());
    fflush(stdout);

    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleA(NULL);
    window_class.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    window_class.lpszClassName = "WoWPS5WindowTest";
    check(RegisterClassExA(&window_class) != 0, "RegisterClassEx");

    desktop = GetDesktopWindow();
    check(desktop != NULL, "GetDesktopWindow");
    desktop_tid = GetWindowThreadProcessId(desktop, &desktop_pid);
    printf("INFO: desktop window %p owned by thread %lu of process %lu (%s)\n", (void*)desktop, desktop_tid, desktop_pid,
           !desktop_tid ? "no thread" : desktop_pid != GetCurrentProcessId() ? "another process" :
           desktop_tid == GetCurrentThreadId() ? "this thread" : "another thread of this process");

    if(EnumDisplayDevicesA(NULL, 0, &device, 0))
        printf("INFO: display device %s \"%s\" flags %#lx\n", device.DeviceName, device.DeviceString, device.StateFlags);
    else printf("INFO: EnumDisplayDevices found no device\n");
    if(EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &mode))
        printf("INFO: current mode %lux%lu %lu bpp %lu Hz\n", mode.dmPelsWidth, mode.dmPelsHeight, mode.dmBitsPerPel, mode.dmDisplayFrequency);
    else printf("INFO: EnumDisplaySettings failed\n");
    printf("INFO: screen metrics %dx%d, virtual %dx%d\n", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
           GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    check(GetSystemMetrics(SM_CXSCREEN) > 0 && GetSystemMetrics(SM_CYSCREEN) > 0, "the screen has a size");

    glyphs = text_pixels();
    printf("INFO: text rendering lit %d pixels in a memory bitmap\n", glyphs);
    check(glyphs > 100, "GDI renders text into a memory bitmap");

    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    SetLastError(0);
    window = CreateWindowExA(0, window_class.lpszClassName, "WoWPS5 window test", WS_OVERLAPPEDWINDOW,
                             100, 100, rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, window_class.hInstance, NULL);
    check(window != NULL, "CreateWindowEx");
    if(!window) {
        printf("WoWPS5 window test FAIL (%d failures)\n", failures);
        return failures;
    }
    check(creates == 1, "WM_CREATE was delivered during CreateWindowEx");
    ShowWindow(window, SW_SHOW);
    check(UpdateWindow(window), "UpdateWindow");
    check(IsWindowVisible(window), "the window is visible");
    check(SetTimer(window, 1, 100, NULL) != 0, "SetTimer 100 ms");
    SetForegroundWindow(window);
    SetFocus(window);
    {
        /* raw mouse input, what a game uses for mouse look */
        RAWINPUTDEVICE raw = {1 /* generic desktop */, 2 /* mouse */, 0, window};
        check(RegisterRawInputDevices(&raw, 1, sizeof raw), "RegisterRawInputDevices for the mouse");
    }

    start = GetTickCount();
    for(;;) {
        DWORD elapsed = GetTickCount() - start;
        if(elapsed >= run_ms && !closed) {
            closed = TRUE;
            /* read back what WM_PAINT drew, through a window DC */
            dc = GetDC(window);
            pixel = GetPixel(dc, 80, 80);
            printf("INFO: pixel at (80,80) is %06lx, at (10,300) is %06lx\n", pixel, GetPixel(dc, 10, 300));
            check(pixel == MARK_COLOR, "the window surface holds what WM_PAINT drew (yellow square)");
            check(GetPixel(dc, 10, 300) == FILL_COLOR, "the window surface holds the background fill");
            ReleaseDC(window, dc);
            GetWindowRect(window, &window_rect);
            GetClientRect(window, &client_rect);
            printf("INFO: window rect (%ld,%ld)-(%ld,%ld), client %dx%d, foreground %p focus %p (window %p)\n",
                   window_rect.left, window_rect.top, window_rect.right, window_rect.bottom,
                   client_width, client_height, (void*)GetForegroundWindow(), (void*)GetFocus(), (void*)window);
            KillTimer(window, 1);
            DestroyWindow(window);
        }
        if(elapsed >= run_ms / 2 && !input_sent) {
            /* synthetic input: goes through the server's input queue to the foreground window */
            input_sent = TRUE;
            memset(input, 0, sizeof input);
            input[0].type = INPUT_KEYBOARD; input[0].ki.wVk = 'A';
            input[1].type = INPUT_KEYBOARD; input[1].ki.wVk = 'A'; input[1].ki.dwFlags = KEYEVENTF_KEYUP;
            check(SendInput(2, input, sizeof input[0]) == 2, "SendInput keyboard");
            GetWindowRect(window, &window_rect);
            SetCursorPos(window_rect.left + 200, window_rect.top + 200);
            memset(input, 0, sizeof input);
            input[0].type = INPUT_MOUSE; input[0].mi.dx = 5; input[0].mi.dy = 5; input[0].mi.dwFlags = MOUSEEVENTF_MOVE;
            check(SendInput(1, input, sizeof input[0]) == 1, "SendInput mouse");
        }
        if(MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT) == WAIT_FAILED) break;
        while(PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
            if(message.message == WM_QUIT) goto done;
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        if(GetTickCount() - start > run_ms + 10000) { printf("FAIL: no WM_QUIT 10 s after DestroyWindow\n"); failures++; break; }
    }
done:
    printf("INFO: WM_CREATE %d, WM_SIZE %d, WM_ERASEBKGND %d, WM_PAINT %d, WM_TIMER %d, WM_KEYDOWN %d, WM_CHAR %d, WM_MOUSEMOVE %d, WM_INPUT %d, WM_DESTROY %d\n",
           creates, sizes, erases, paints, timers, key_downs, chars, mouse_moves, raw_inputs, destroys);
    check(message.message == WM_QUIT && message.wParam == 7, "WM_QUIT carried the PostQuitMessage code");
    /* a window manager may give the window another size than the 640x360 asked for */
    if(client_width != 640 || client_height != 360)
        printf("INFO: the client area is %dx%d, not the 640x360 asked for (window manager)\n", client_width, client_height);
    check(sizes >= 1 && client_width == client_rect.right && client_height == client_rect.bottom && client_width > 0,
          "WM_SIZE reported the client area GetClientRect returns");
    check(paints >= 2, "WM_PAINT arrived, and again after InvalidateRect");
    check(timers >= (int)(run_ms / 100) * 7 / 10, "WM_TIMER arrived at about 10 Hz");
    check(key_downs >= 1 && chars >= 1, "keyboard input reached the window (WM_KEYDOWN, WM_CHAR)");
    check(mouse_moves >= 1, "mouse input reached the window (WM_MOUSEMOVE)");
    check(raw_inputs >= 1, "raw mouse input reached the window (WM_INPUT)");
    check(destroys == 1 && window_thread == GetCurrentThreadId(), "WM_DESTROY on the creating thread");

    printf("WoWPS5 window test %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    fflush(stdout);
    return failures;
}
