/* Exercise the same Windows TLS and certificate-chain path as a client.
 * MIT license. Never ignore a certificate error. */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>

static int request(HINTERNET session, const WCHAR *host, int valid)
{
    HINTERNET connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = connection ? WinHttpOpenRequest(connection, L"GET", L"/", NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    DWORD error = 0, status = 0, bytes = sizeof(status);
    BOOL success = req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, NULL);
    if (!success) error = GetLastError();
    if (success) success = WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, WINHTTP_NO_HEADER_INDEX);
    int passed = valid ? (success && status >= 200 && status < 600) :
        (!success && (error == ERROR_WINHTTP_SECURE_FAILURE || error == ERROR_WINHTTP_SECURE_CHANNEL_ERROR));
    printf("%s: %ls %s (HTTP %lu, error %lu)\n", passed ? "PASS" : "FAIL", host,
        valid ? "verified HTTPS response" : "untrusted certificate rejected", status, error);
    fflush(stdout);
    if (req) WinHttpCloseHandle(req);
    if (connection) WinHttpCloseHandle(connection);
    return !passed;
}

int main(void)
{
    HINTERNET session = WinHttpOpen(L"WoWPS5 TLS test", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { printf("FAIL: WinHttpOpen error %lu\n", GetLastError()); return 1; }
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 15000);
    int failures = request(session, L"example.com", 1);
    /* Same TLS 1.2 server family as the negative cases. Wine maps certificate
     * failures to SECURE_CHANNEL_ERROR; the trace identifies CA/CN rejection. */
    failures += request(session, L"badssl.com", 1);
    failures += request(session, L"self-signed.badssl.com", 0);
    failures += request(session, L"wrong.host.badssl.com", 0);
    WinHttpCloseHandle(session);
    printf("TLS test ended: %d failures\n", failures);
    return failures;
}
