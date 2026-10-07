/* A real, CRT-free Windows x64 test image. MIT license.
 * Tests DIR64 relocations, zero-filled data, IAT dispatch and the Windows ABI.
 * Its entry returns to the test host; it is not a standalone desktop app. */
#include <windows.h>
static volatile unsigned long long state;
static volatile unsigned long long* volatile relocated = &state;
unsigned long long WoWPS5Smoke(void) {
    if (*relocated != 0) return 1;
    unsigned long long before=GetTickCount64();
    Sleep(12);
    unsigned long long after=GetTickCount64();
    if(after<before || after-before<10 || after-before>10000) return 2;
    *relocated=0x123456789abcdef0ULL;
    if(state!=0x123456789abcdef0ULL) return 3;
    return 42;
}
