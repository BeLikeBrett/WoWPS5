/* TCP self-connect wake-up sockets, as used by WowB.exe. MIT license.
 * A non-blocking socket binds loopback, connects to its own port, and is
 * duplicated. Test both immediate receives and select/WSAPoll readiness.
 * The console runner's deadline bounds a broken non-blocking receive. */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int failures;
static int check(int ok, const char *what) {
    printf("%s: %s (error %d)\n", ok ? "PASS" : "FAIL", what, ok ? 0 : WSAGetLastError());
    fflush(stdout);
    if (!ok) failures++;
    return ok;
}

int main(int argc, char **argv) {
    WSADATA data;
    SOCKET sock = INVALID_SOCKET, copy = INVALID_SOCKET;
    WSAPROTOCOL_INFO protocol;
    struct sockaddr_in address = {0};
    int length = sizeof(address), result;
    u_long on = 1;
    char byte = 0;
    fd_set set;
    struct timeval limit = {2, 0};
    WSAPOLLFD entry;
    DWORD began;
    int expect_refusal = argc == 2 && !strcmp(argv[1], "--expect-refusal");
    if (!check(!WSAStartup(MAKEWORD(2, 2), &data), "WSAStartup")) return 1;
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!check(sock != INVALID_SOCKET && !ioctlsocket(sock, FIONBIO, &on), "non-blocking TCP socket")) goto done;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (!check(!bind(sock, (struct sockaddr *)&address, length) &&
               !getsockname(sock, (struct sockaddr *)&address, &length), "bind loopback, discover port")) goto done;
    printf("self-connect port %u\n", ntohs(address.sin_port)); fflush(stdout);
    result = connect(sock, (struct sockaddr *)&address, length);
    if (expect_refusal) {
        check(result == SOCKET_ERROR && WSAGetLastError() == WSAECONNREFUSED,
              "unsupported self-connect is refused so the application can fall back");
        goto done;
    }
    if (!check(!result || WSAGetLastError() == WSAEWOULDBLOCK, "connect to the same socket's port")) goto done;
    FD_ZERO(&set); FD_SET(sock, &set);
    if (!check(select(0, NULL, &set, NULL, &limit) == 1, "self-connect becomes writable")) goto done;
    result = 0; length = sizeof(result);
    if (!check(!getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&result, &length) && !result, "self-connect has no socket error")) goto done;
    if (!check(!WSADuplicateSocket(sock, GetCurrentProcessId(), &protocol), "duplicate socket protocol")) goto done;
    copy = WSASocket(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &protocol, 0, WSA_FLAG_OVERLAPPED);
    if (!check(copy != INVALID_SOCKET, "open duplicated socket handle")) goto done;
    printf("checking empty original receive\n"); fflush(stdout);
    began = GetTickCount(); result = recv(sock, &byte, 1, 0);
    check(result == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK && GetTickCount() - began < 1000,
          "empty original receive returns WSAEWOULDBLOCK immediately");
    printf("checking empty duplicate receive\n"); fflush(stdout);
    began = GetTickCount(); result = recv(copy, &byte, 1, 0);
    check(result == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK && GetTickCount() - began < 1000,
          "empty duplicate receive returns WSAEWOULDBLOCK immediately");
    if (!check(send(copy, "a", 1, 0) == 1, "send wake-up byte through duplicate")) goto done;
    FD_ZERO(&set); FD_SET(sock, &set); limit.tv_sec = 2; limit.tv_usec = 0;
    check(select(0, &set, NULL, NULL, &limit) == 1, "original becomes readable after wake-up");
    if (!check(recv(sock, &byte, 1, 0) == 1 && byte == 'a', "original receives wake-up byte")) goto done;
    if (!check(send(sock, "b", 1, 0) == 1, "send second wake-up byte")) goto done;
    entry.fd = copy; entry.events = POLLRDNORM; entry.revents = 0;
    if (!check(WSAPoll(&entry, 1, 2000) == 1 && (entry.revents & POLLRDNORM), "WSAPoll on duplicate sees wake-up")) goto done;
    check(recv(copy, &byte, 1, 0) == 1 && byte == 'b', "duplicate receives wake-up byte");
done:
    if (copy != INVALID_SOCKET) closesocket(copy);
    if (sock != INVALID_SOCKET) closesocket(sock);
    WSACleanup();
    printf("WoWPS5 self-connect test %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures;
}
