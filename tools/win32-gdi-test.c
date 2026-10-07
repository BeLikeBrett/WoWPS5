/* user32 and gdi32 without a window. MIT license.
 * Drawing into a memory bitmap needs Wine's win32u (its Unix side holds the
 * DIB engine and the system parameters) but no display driver, so it is the
 * first check that win32u works at all. Window creation is reported, not
 * judged: it needs the desktop and a display driver. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int failures;
static void check(int passed, const char* what) {
    if(!passed) failures++;
    printf("%s: %s\n", passed ? "PASS" : "FAIL", what);
    fflush(stdout);
}
static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) { return DefWindowProcA(window, message, w, l); }

int main(void) {
    char text[64];
    wsprintfA(text, "%d-%s", 42, "wow");
    check(!strcmp(text, "42-wow") && !lstrcmpiA("WoW", "wow") && CharUpperA((char*)(ULONG_PTR)'a') == (char*)(ULONG_PTR)'A', "user32 string functions");

    printf("info screen %d x %d, %d monitors\n", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), GetSystemMetrics(SM_CMONITORS));
    check(GetSysColor(COLOR_WINDOW) != 0 || GetSysColor(COLOR_WINDOWTEXT) == 0, "system colours");

    BITMAPINFO info; DWORD* bits = NULL;
    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(info.bmiHeader); info.bmiHeader.biWidth = 256; info.bmiHeader.biHeight = -256;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, (void**)&bits, NULL, 0) : NULL;
    check(dc && bitmap && bits && SelectObject(dc, bitmap), "a memory DC and a 256x256 DIB section");
    if(dc && bitmap && bits) {
        RECT all = { 0, 0, 256, 256 }, box = { 64, 64, 192, 192 };
        HBRUSH red = CreateSolidBrush(RGB(255, 0, 0)), blue = CreateSolidBrush(RGB(0, 0, 255));
        FillRect(dc, &all, blue); FillRect(dc, &box, red);
        check(bits[0] == 0x000000ff && bits[128 * 256 + 128] == 0x00ff0000 && GetPixel(dc, 10, 10) == RGB(0, 0, 255), "FillRect paints the bitmap's own memory");
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0, 255, 0)); HGDIOBJ old = SelectObject(dc, pen);
        MoveToEx(dc, 0, 0, NULL); LineTo(dc, 256, 256);
        SelectObject(dc, old);
        check(GetPixel(dc, 100, 100) == RGB(0, 255, 0) && GetPixel(dc, 100, 101) == RGB(255, 0, 0), "a line through the DIB engine");
        HDC other = CreateCompatibleDC(dc); DWORD* copy = NULL;
        HBITMAP second = CreateDIBSection(other, &info, DIB_RGB_COLORS, (void**)&copy, NULL, 0);
        SelectObject(other, second);
        check(BitBlt(other, 0, 0, 128, 128, dc, 64, 64, SRCCOPY) && copy && copy[10] == 0x00ff0000 && copy[36 * 256 + 36] == 0x0000ff00, "BitBlt between two bitmaps");
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        const BOOL drawn = TextOutA(dc, 4, 200, "WoWPS5", 6);
        int lit = 0;
        for(int y = 200; y < 232; y++) for(int x = 0; x < 128; x++) lit += bits[y * 256 + x] == 0x00ffffff;
        TEXTMETRICA metrics; GetTextMetricsA(dc, &metrics); GetTextFaceA(dc, sizeof(text), text);
        printf("info TextOut returned %d and lit %d pixels; font \"%s\" height %ld\n", drawn, lit, text, metrics.tmHeight);
        DeleteObject(second); DeleteDC(other); DeleteObject(pen); DeleteObject(red); DeleteObject(blue);
    }
    if(bitmap) DeleteObject(bitmap);
    if(dc) DeleteDC(dc);

    WNDCLASSA class; memset(&class, 0, sizeof(class));
    class.lpfnWndProc = procedure; class.hInstance = GetModuleHandleA(NULL); class.lpszClassName = "WoWPS5GdiTest";
    check(RegisterClassA(&class) != 0, "a window class registers");
    SetLastError(0);
    HWND window = CreateWindowExA(0, "WoWPS5GdiTest", "test", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, NULL, NULL, class.hInstance, NULL);
    printf("info CreateWindowEx returned %p, error %lu\n", (void*)window, GetLastError());
    if(window) DestroyWindow(window);

    printf(failures ? "WoWPS5 Win32 gdi test FAIL\n" : "WoWPS5 Win32 gdi test PASS\n");
    return failures;
}
