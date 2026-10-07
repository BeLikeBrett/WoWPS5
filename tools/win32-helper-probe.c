/* What a Wine without helper processes answers. MIT license.
 *
 *   win32-helper-probe.exe            every probe below
 *   win32-helper-probe.exe crash      dereference a null pointer (what the unhandled-exception path does)
 *
 * This is a measurement, not a pass/fail test: each line is
 *     <helper it depends on> | <call> | <result>
 * Run it under a normal multi-process Wine and under the single-process build
 * and compare. Helpers: wineboot (volatile registry keys made at session
 * start, and it starts services.exe), services.exe (service control manager),
 * winedevice.exe (kernel drivers: mountmgr.sys, nsiproxy.sys, winebus.sys and
 * the HID stack), plugplay.exe, rpcss.exe (COM across apartments/processes),
 * conhost.exe (consoles), start.exe/explorer.exe (ShellExecute). */
#define COBJMACROS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>
#include <xinput.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>

static void line(const char* helper, const char* call, const char* format, ...) {
    va_list args;
    printf("%-11s | %-44s | ", helper, call);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

static void registry_value(const char* helper, HKEY root, const char* key, const char* value) {
    char data[256] = "", call[160];
    DWORD size = sizeof data - 1, type = 0;
    LONG status = RegGetValueA(root, key, value, RRF_RT_ANY, &type, data, &size);
    snprintf(call, sizeof call, "%.30s...\\%s", key, value);
    if(status) line(helper, call, "error %ld", status);
    else if(type == REG_DWORD) line(helper, call, "%lu", *(DWORD*)data);
    else line(helper, call, "\"%s\"", data);
}

static DWORD WINAPI unmarshal_thread(void* argument) {
    IStream* stream = argument;
    IUnknown* object = NULL;
    HRESULT result;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    result = CoGetInterfaceAndReleaseStream(stream, &IID_IUnknown, (void**)&object);
    if(SUCCEEDED(result) && object) IUnknown_Release(object);
    CoUninitialize();
    return (DWORD)result;
}

int main(int argc, char** argv) {
    char text[512], name[256];
    DWORD size, serial = 0, flags = 0, code = 0, start;
    ULARGE_INTEGER free_bytes = {0}, total_bytes = {0};
    HANDLE handle;
    SC_HANDLE manager;
    WSADATA wsa;
    HRESULT result;

    if(argc > 1 && !strcmp(argv[1], "crash")) {
        printf("dereferencing a null pointer\n");
        fflush(stdout);
        *(volatile int*)0 = 1;
        return 0;
    }

    /* session start: wineboot makes these volatile keys every time the server starts */
    registry_value("wineboot", HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString");
    registry_value("wineboot", HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "~MHz");
    registry_value("wineboot", HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS", "SystemProductName");
    registry_value("wineboot", HKEY_LOCAL_MACHINE, "System\\CurrentControlSet\\Control\\ComputerName\\ActiveComputerName", "ComputerName");
    registry_value("wineboot", HKEY_LOCAL_MACHINE, "System\\CurrentControlSet\\Control\\Session Manager\\Environment", "NUMBER_OF_PROCESSORS");
    registry_value("wineboot", HKEY_CURRENT_USER, "Volatile Environment", "USERPROFILE");
    {
        /* KUSER_SHARED_DATA, read the way programs that bypass the API read it */
        const unsigned char* shared = (const unsigned char*)0x7ffe0000;
        char root[32];
        int i;
        for(i = 0; i < 31; i++) root[i] = (char)*(const WCHAR*)(shared + 0x30 + 2 * i);
        root[31] = 0;
        line("wineboot", "shared data NtMajorVersion.Minor.Build", "%lu.%lu.%lu", *(const ULONG*)(shared + 0x26c),
             *(const ULONG*)(shared + 0x270), *(const ULONG*)(shared + 0x260));
        line("wineboot", "shared data NtProductType, NtSystemRoot", "%lu, \"%s\"", *(const ULONG*)(shared + 0x264), root);
        line("-", "shared data ProcessorFeatures SSE2, AVX2", "%d, %d", IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE),
             IsProcessorFeaturePresent(40 /* PF_AVX2_INSTRUCTIONS_AVAILABLE */));
    }
    {
        OSVERSIONINFOEXA version = {sizeof version};
        GetVersionExA((OSVERSIONINFOA*)&version);
        line("-", "GetVersionEx", "%lu.%lu.%lu", version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);
    }
    size = sizeof name;
    if(GetComputerNameA(name, &size)) line("wineboot", "GetComputerName", "\"%s\"", name);
    else line("wineboot", "GetComputerName", "error %lu", GetLastError());
    size = sizeof name;
    if(GetUserNameA(name, &size)) line("-", "GetUserName", "\"%s\"", name);
    else line("-", "GetUserName", "error %lu", GetLastError());
    size = GetEnvironmentVariableA("NUMBER_OF_PROCESSORS", name, sizeof name);
    if(size) line("wineboot", "environment NUMBER_OF_PROCESSORS", "\"%s\"", name);
    else line("wineboot", "environment NUMBER_OF_PROCESSORS", "not set");
    size = GetEnvironmentVariableA("USERPROFILE", name, sizeof name);
    if(size) line("wineboot", "environment USERPROFILE", "\"%s\"", name);
    else line("wineboot", "environment USERPROFILE", "not set");
    result = SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, text);
    if(SUCCEEDED(result)) line("-", "SHGetFolderPath(CSIDL_APPDATA)", "\"%s\"", text);
    else line("-", "SHGetFolderPath(CSIDL_APPDATA)", "error %#lx", result);

    /* services.exe */
    manager = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if(manager) { line("services", "OpenSCManager", "ok"); CloseServiceHandle(manager); }
    else line("services", "OpenSCManager", "error %lu", GetLastError());

    /* winedevice: mountmgr.sys */
    handle = CreateFileA("\\\\.\\MountPointManager", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if(handle != INVALID_HANDLE_VALUE) { line("mountmgr", "CreateFile \\\\.\\MountPointManager", "ok"); CloseHandle(handle); }
    else line("mountmgr", "CreateFile \\\\.\\MountPointManager", "error %lu", GetLastError());
    line("mountmgr", "GetDriveType C:\\", "%u (3 is DRIVE_FIXED, 1 is DRIVE_NO_ROOT_DIR)", GetDriveTypeA("C:\\"));
    line("mountmgr", "GetLogicalDrives", "%#lx", GetLogicalDrives());
    if(GetVolumeInformationA("C:\\", name, sizeof name, &serial, &size, &flags, text, sizeof text))
        line("mountmgr", "GetVolumeInformation C:\\", "label \"%s\" serial %08lx filesystem \"%s\"", name, serial, text);
    else line("mountmgr", "GetVolumeInformation C:\\", "error %lu", GetLastError());
    if(QueryDosDeviceA("C:", text, sizeof text)) line("mountmgr", "QueryDosDevice C:", "\"%s\"", text);
    else line("mountmgr", "QueryDosDevice C:", "error %lu", GetLastError());
    if(GetVolumeNameForVolumeMountPointA("C:\\", text, sizeof text)) line("mountmgr", "GetVolumeNameForVolumeMountPoint C:\\", "\"%s\"", text);
    else line("mountmgr", "GetVolumeNameForVolumeMountPoint C:\\", "error %lu", GetLastError());
    if(GetDiskFreeSpaceExA("C:\\", &free_bytes, &total_bytes, NULL))
        line("-", "GetDiskFreeSpaceEx C:\\", "%llu MiB free of %llu MiB", free_bytes.QuadPart >> 20, total_bytes.QuadPart >> 20);
    else line("-", "GetDiskFreeSpaceEx C:\\", "error %lu", GetLastError());

    /* winedevice: nsiproxy.sys */
    {
        ULONG length = 0, status = GetAdaptersAddresses(AF_UNSPEC, 0, NULL, NULL, &length);
        line("nsiproxy", "GetAdaptersAddresses (size query)", "status %lu, %lu bytes needed (111 is ERROR_BUFFER_OVERFLOW: adapters exist)", status, length);
        length = 0;
        status = GetIfTable(NULL, &length, FALSE);
        line("nsiproxy", "GetIfTable (size query)", "status %lu, %lu bytes needed", status, length);
    }
    if(!WSAStartup(MAKEWORD(2, 2), &wsa)) {
        struct addrinfo hints = {0}, *info = NULL;
        struct sockaddr_in address = {0};
        SOCKET s, listener;
        int status, length = sizeof address;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        status = getaddrinfo("localhost", "80", &hints, &info);
        if(status) line("-", "getaddrinfo localhost", "error %d", status);
        else line("-", "getaddrinfo localhost", "ok");
        if(info) freeaddrinfo(info);
        info = NULL;
        status = gethostname(name, sizeof name);
        if(status) line("-", "gethostname", "error %d", WSAGetLastError());
        else line("-", "gethostname", "\"%s\"", name);
        if(!status) {
            status = getaddrinfo(name, NULL, &hints, &info);
            if(status) line("nsiproxy", "getaddrinfo <own host name>", "error %d", status);
            else line("nsiproxy", "getaddrinfo <own host name>", "ok");
            if(info) freeaddrinfo(info);
        }
        /* a TCP connection to ourselves: sockets are the server's, no helper involved */
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if(listener == INVALID_SOCKET || bind(listener, (struct sockaddr*)&address, sizeof address) || listen(listener, 1)
           || getsockname(listener, (struct sockaddr*)&address, &length))
            line("-", "TCP listen on 127.0.0.1", "error %d", WSAGetLastError());
        else {
            char byte = 'x', back = 0;
            SOCKET accepted;
            s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            status = connect(s, (struct sockaddr*)&address, sizeof address);
            accepted = status ? INVALID_SOCKET : accept(listener, NULL, NULL);
            if(accepted != INVALID_SOCKET && send(s, &byte, 1, 0) == 1 && recv(accepted, &back, 1, 0) == 1 && back == 'x')
                line("-", "TCP connect/accept/send/recv on loopback", "ok");
            else line("-", "TCP connect/accept/send/recv on loopback", "error %d", WSAGetLastError());
            closesocket(accepted);
            closesocket(s);
        }
        closesocket(listener);
        WSACleanup();
    }
    else line("-", "WSAStartup", "error %d", WSAGetLastError());

    /* winedevice: winebus.sys and the HID stack; plugplay */
    {
        XINPUT_STATE state;
        UINT devices = 0;
        DWORD status = XInputGetState(0, &state);
        line("winebus", "XInputGetState(0)", "%lu (1167 is ERROR_DEVICE_NOT_CONNECTED)", status);
        if(GetRawInputDeviceList(NULL, &devices, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1)
            line("winebus", "GetRawInputDeviceList", "error %lu", GetLastError());
        else line("winebus", "GetRawInputDeviceList", "%u devices", devices);
    }

    /* rpcss */
    result = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    line("-", "CoInitializeEx", "%#lx", result);
    {
        IShellLinkA* link = NULL;
        IStream* stream = NULL;
        IRunningObjectTable* table = NULL;
        IMoniker* moniker = NULL;
        HANDLE thread;
        MSG message;
        result = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkA, (void**)&link);
        line("-", "CoCreateInstance in-process (ShellLink)", "%#lx", result);
        if(link) {
            result = CoMarshalInterThreadInterfaceInStream(&IID_IUnknown, (IUnknown*)link, &stream);
            line("rpcss", "CoMarshalInterThreadInterfaceInStream", "%#lx", result);
            if(SUCCEEDED(result)) {
                thread = CreateThread(NULL, 0, unmarshal_thread, stream, 0, NULL);
                start = GetTickCount();
                while(MsgWaitForMultipleObjects(1, &thread, FALSE, 15000, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
                    while(PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&message);
                    if(GetTickCount() - start > 15000) break;
                }
                if(WaitForSingleObject(thread, 0) == WAIT_OBJECT_0) {
                    GetExitCodeThread(thread, &code);
                    line("rpcss", "unmarshal in another apartment", "%#lx after %lu ms", code, GetTickCount() - start);
                }
                else line("rpcss", "unmarshal in another apartment", "no answer after %lu ms", GetTickCount() - start);
                CloseHandle(thread);
            }
            result = GetRunningObjectTable(0, &table);
            if(SUCCEEDED(result) && SUCCEEDED(result = CreateItemMoniker(L"!", L"WoWPS5Probe", &moniker))) {
                DWORD cookie = 0;
                result = IRunningObjectTable_Register(table, 0, (IUnknown*)link, moniker, &cookie);
                if(SUCCEEDED(result)) IRunningObjectTable_Revoke(table, cookie);
            }
            line("rpcss", "running object table Register", "%#lx", result);
            if(moniker) IMoniker_Release(moniker);
            if(table) IRunningObjectTable_Release(table);
            IShellLinkA_Release(link);
        }
    }
    CoUninitialize();

    /* other programs */
    {
        STARTUPINFOA startup = {sizeof startup};
        PROCESS_INFORMATION process;
        char command[] = "C:\\windows\\system32\\cmd.exe /c exit 0";
        if(CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
            WaitForSingleObject(process.hProcess, 10000);
            line("(process)", "CreateProcess cmd.exe /c exit 0", "ok");
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
        else line("(process)", "CreateProcess cmd.exe /c exit 0", "error %lu (50 is ERROR_NOT_SUPPORTED)", GetLastError());
    }

    /* conhost */
    {
        DWORD mode = 0;
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        line("conhost", "standard output", "handle %p, type %lu (1 disk, 2 character, 3 pipe), console mode %s",
             (void*)output, GetFileType(output), GetConsoleMode(output, &mode) ? "readable" : "not a console");
        line("conhost", "GetConsoleWindow", "%p", (void*)GetConsoleWindow());
    }

    /* no helper: in-process facilities a game start touches */
    {
        unsigned char random[16] = {0};
        NTSTATUS status = BCryptGenRandom(NULL, random, sizeof random, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        line("-", "BCryptGenRandom", "status %#lx, first bytes %02x%02x%02x%02x", status, random[0], random[1], random[2], random[3]);
    }
    printf("probe done\n");
    fflush(stdout);
    return 0;
}
