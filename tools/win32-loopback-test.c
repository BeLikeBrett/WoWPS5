/* Loopback socket pairs as a game's network layer makes them. MIT license.
 * For IPv4 and IPv6: listen on the loopback address, connect to it, accept,
 * and pass a byte each way. Every step that could wait is given five seconds
 * and reports where it stopped. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>

static int failures;
static void say(BOOL passed, const char* family, const char* what) {
    const int error = WSAGetLastError();
    if (!passed) failures++;
    printf("%s: %s %s (error %d)\n", passed ? "PASS" : "FAIL", family, what, passed ? 0 : error); fflush(stdout);
}
static BOOL ready(SOCKET s, BOOL writing) {
    fd_set set; FD_ZERO(&set); FD_SET(s, &set); struct timeval limit = { 5, 0 };
    return select(0, writing ? NULL : &set, writing ? &set : NULL, NULL, &limit) == 1;
}
static void pair(int family, const char* name, BOOL nonblocking) {
    union { struct sockaddr any; struct sockaddr_in v4; struct sockaddr_in6 v6; } address; int length;
    memset(&address, 0, sizeof(address));
    if (family == AF_INET) { address.v4.sin_family = AF_INET; address.v4.sin_addr.s_addr = htonl(INADDR_LOOPBACK); length = sizeof(address.v4); }
    else { address.v6.sin6_family = AF_INET6; address.v6.sin6_addr = in6addr_loopback; length = sizeof(address.v6); }
    SOCKET listener = socket(family, SOCK_STREAM, IPPROTO_TCP);
    say(listener != INVALID_SOCKET, name, "socket"); if (listener == INVALID_SOCKET) return;
    BOOL ok = bind(listener, &address.any, length) == 0; say(ok, name, "bind to the loopback address, any port"); if (!ok) { closesocket(listener); return; }
    ok = listen(listener, 1) == 0 && getsockname(listener, &address.any, &length) == 0; say(ok, name, "listen, getsockname");
    SOCKET client = socket(family, SOCK_STREAM, IPPROTO_TCP); u_long on = 1;
    if (nonblocking) ioctlsocket(client, FIONBIO, &on);
    int result = connect(client, &address.any, length);
    ok = result == 0 || (nonblocking && WSAGetLastError() == WSAEWOULDBLOCK && ready(client, TRUE));
    say(ok, name, nonblocking ? "non-blocking connect, writable within 5 s" : "blocking connect");
    ok = ok && ready(listener, FALSE); say(ok, name, "the listener is readable within 5 s");
    SOCKET server = ok ? accept(listener, NULL, NULL) : INVALID_SOCKET; say(server != INVALID_SOCKET, name, "accept");
    char byte = 0;
    ok = server != INVALID_SOCKET && send(client, "a", 1, 0) == 1 && ready(server, FALSE) && recv(server, &byte, 1, 0) == 1 && byte == 'a';
    say(ok, name, "a byte from the connecting side arrives");
    ok = server != INVALID_SOCKET && send(server, "b", 1, 0) == 1 && ready(client, FALSE) && recv(client, &byte, 1, 0) == 1 && byte == 'b';
    say(ok, name, "and one back");
    /* nothing more is sent: a wait for data has to end when its time is up */
    if (server != INVALID_SOCKET) {
        fd_set set; FD_ZERO(&set); FD_SET(server, &set); struct timeval limit = { 1, 0 };
        const DWORD before = GetTickCount(); const int found = select(0, &set, NULL, NULL, &limit); const DWORD took = GetTickCount() - before;
        printf("     select on an idle socket returned %d after %lu ms\n", found, took);
        say(found == 0 && took >= 900 && took < 3000, name, "select on an idle socket times out after its second");
        WSAPOLLFD entry = { server, POLLRDNORM, 0 };
        const DWORD start = GetTickCount(); const int polled = WSAPoll(&entry, 1, 500); const DWORD lasted = GetTickCount() - start;
        printf("     WSAPoll on it returned %d after %lu ms\n", polled, lasted);
        say(polled == 0 && lasted >= 400 && lasted < 2500, name, "WSAPoll on it times out after its half second");
    }
    if (server != INVALID_SOCKET) closesocket(server);
    closesocket(client); closesocket(listener);
}
int main(void) {
    WSADATA data; if (WSAStartup(MAKEWORD(2, 2), &data)) { printf("FAIL: WSAStartup\n"); return 1; }
    pair(AF_INET, "IPv4", FALSE); pair(AF_INET, "IPv4 non-blocking", TRUE);
    pair(AF_INET6, "IPv6", FALSE); pair(AF_INET6, "IPv6 non-blocking", TRUE);
    SOCKET dual = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP); DWORD off = 0;
    say(dual != INVALID_SOCKET && setsockopt(dual, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&off, sizeof(off)) == 0, "IPv6", "a socket that takes IPv4 as well (IPV6_V6ONLY off)");
    if (dual != INVALID_SOCKET) closesocket(dual);
    SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); say(udp != INVALID_SOCKET, "IPv4", "a UDP socket"); if (udp != INVALID_SOCKET) closesocket(udp);
    udp = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP); say(udp != INVALID_SOCKET, "IPv6", "a UDP socket"); if (udp != INVALID_SOCKET) closesocket(udp);
    printf(failures ? "WoWPS5 loopback test FAIL (%d)\n" : "WoWPS5 loopback test PASS\n", failures);
    return failures;
}
