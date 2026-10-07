/* Real window receives remote Unicode, ordinary keys and pointer events.
 * MIT license. Uses a blocked message wait to exercise input wake-ups. */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

static WCHAR text[32];
static unsigned chars, tabs, clicks;
static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_CHAR && w != '\t' && chars < 31) text[chars++] = w;
    if (message == WM_KEYDOWN && w == VK_TAB) tabs++;
    if (message == WM_LBUTTONDOWN) clicks++;
    if (chars == 8 && tabs && clicks) PostQuitMessage(0);
    if (message == WM_TIMER) PostQuitMessage(1);
    return DefWindowProcW(window, message, w, l);
}

int main(void)
{
    WNDCLASSW cls = {.lpfnWndProc = procedure, .hInstance = GetModuleHandleW(NULL), .lpszClassName = L"ConsoleInputTest"};
    RegisterClassW(&cls);
    HWND window = CreateWindowW(cls.lpszClassName, L"WoWPS5 encrypted input test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        0, 0, 1920, 1080, NULL, NULL, cls.hInstance, NULL);
    if (!window) { printf("FAIL: input test window error %lu\n", GetLastError()); return 1; }
    SetForegroundWindow(window); SetFocus(window);
    SetTimer(window, 1, 30000, NULL);
    printf("INPUT TEST READY\n"); fflush(stdout);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    int valid = !wcscmp(text, L"HelloWoW") && tabs && clicks;
    printf("%s: remote Unicode, TAB and mouse reached the foreground window (characters=%u, tabs=%u, clicks=%u)\n",
        valid ? "PASS" : "FAIL", chars, tabs, clicks);
    fflush(stdout);
    DestroyWindow(window);
    return !valid;
}
