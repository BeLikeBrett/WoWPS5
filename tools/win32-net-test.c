/* Winsock through Wine. MIT license.
 * Loopback first (no network needed), then name resolution and one outbound
 * TCP connection, which needs the console's network and a resolver.
 *   win32-net-test.exe [host [port]]     default example.com 80 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>

static int failures;
static void check(int passed, const char* what) {
    if(!passed) failures++;
    printf("%s: %s\n", passed ? "PASS" : "FAIL", what);
    fflush(stdout);
}
static DWORD WINAPI echo(void* argument) {
    SOCKET listener = (SOCKET)(ULONG_PTR)argument, peer = accept(listener, NULL, NULL);
    char buffer[64]; int got;
    if(peer == INVALID_SOCKET) return 1;
    while((got = recv(peer, buffer, sizeof(buffer), 0)) > 0) send(peer, buffer, got, 0);
    closesocket(peer);
    return 0;
}

int main(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "example.com";
    const char* port = argc > 2 ? argv[2] : "80";
    WSADATA data;
    check(!WSAStartup(MAKEWORD(2, 2), &data), "WSAStartup");

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    int length = sizeof(address);
    int ok = listener != INVALID_SOCKET && !bind(listener, (struct sockaddr*)&address, sizeof(address)) &&
             !listen(listener, 1) && !getsockname(listener, (struct sockaddr*)&address, &length);
    check(ok, "a listening TCP socket on the loopback");
    HANDLE thread = ok ? CreateThread(NULL, 0, echo, (void*)(ULONG_PTR)listener, 0, NULL) : NULL;
    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    char reply[16] = "";
    ok = thread && client != INVALID_SOCKET && !connect(client, (struct sockaddr*)&address, sizeof(address)) &&
         send(client, "wowps5", 6, 0) == 6 && recv(client, reply, sizeof(reply), 0) == 6 && !memcmp(reply, "wowps5", 6);
    check(ok, "connect, send and receive across the loopback");
    u_long nonblocking = 1; fd_set readable; struct timeval none = { 0, 0 };
    ok = !ioctlsocket(client, FIONBIO, &nonblocking) && recv(client, reply, sizeof(reply), 0) == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK;
    FD_ZERO(&readable); FD_SET(client, &readable);
    check(ok && select(0, &readable, NULL, NULL, &none) == 0, "a non-blocking socket reports WSAEWOULDBLOCK, and select sees nothing to read");
    closesocket(client);
    if(thread) { WaitForSingleObject(thread, 5000); CloseHandle(thread); }
    closesocket(listener);

    char name[256] = "";
    check(!gethostname(name, sizeof(name)) && name[0], "gethostname");
    printf("info host name %s\n", name);

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *found = NULL;
    const int resolved = getaddrinfo(host, port, &hints, &found);
    if(!resolved && found) {
        const unsigned char* bytes = (const unsigned char*)&((struct sockaddr_in*)found->ai_addr)->sin_addr;
        printf("info %s resolves to %u.%u.%u.%u\n", host, bytes[0], bytes[1], bytes[2], bytes[3]);
    } else printf("info getaddrinfo(%s) error %d\n", host, resolved);
    check(!resolved && found, "getaddrinfo resolves a host name");

    if(found) {
        SOCKET remote = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
        char request[256], response[64] = "";
        snprintf(request, sizeof(request), "HEAD / HTTP/1.0\r\nHost: %s\r\n\r\n", host);
        ok = remote != INVALID_SOCKET && !connect(remote, found->ai_addr, (int)found->ai_addrlen) &&
             send(remote, request, (int)strlen(request), 0) > 0 && recv(remote, response, sizeof(response) - 1, 0) > 8;
        response[strcspn(response, "\r\n")] = 0;
        printf("info first line of the reply: %s\n", response);
        check(ok && !memcmp(response, "HTTP/", 5), "an outbound TCP connection gets an HTTP reply");
        if(remote != INVALID_SOCKET) closesocket(remote);
        freeaddrinfo(found);
    } else check(0, "an outbound TCP connection gets an HTTP reply");

    WSACleanup();
    printf(failures ? "WoWPS5 Win32 net test FAIL\n" : "WoWPS5 Win32 net test PASS\n");
    return failures;
}
