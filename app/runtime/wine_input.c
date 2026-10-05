/* Optional encrypted input for Wine on the console. MIT license.
 * A fresh 256-bit PSK is provisioned per run, consumed and deleted at startup.
 * Only authenticated TLS carries events. No input contents are logged. */
#ifdef __PROSPERO__
#include <gnutls/gnutls.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

struct event { uint32_t type, code, flags; int32_t x, y; };
#define EVENT_CAPACITY 4096
static struct event events[EVENT_CAPACITY];
static unsigned first, count;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char secret[32];

int wowps5_input_queue(unsigned type, unsigned code, unsigned flags, int x, int y)
{
    int accepted = 0;
    pthread_mutex_lock(&lock);
    if (count < EVENT_CAPACITY)
    {
        events[(first + count++) % EVENT_CAPACITY] = (struct event){type, code, flags, x, y};
        accepted = 1;
    }
    pthread_mutex_unlock(&lock);
    return accepted;
}

/* GnuTLS's constructor runs before the console platform has loaded modules.
 * Initialize explicitly from Wine/input after platform_init instead. */
int _gnutls_global_init_skip(void) { return 1; }

int wowps5_input_read(unsigned *type, unsigned *code, unsigned *flags, int *x, int *y)
{
    int result = 0;
    pthread_mutex_lock(&lock);
    if (count)
    {
        struct event event = events[first];
        memset(&events[first], 0, sizeof(events[first]));
        first = (first + 1) % EVENT_CAPACITY;
        count--;
        *type = event.type; *code = event.code; *flags = event.flags;
        *x = event.x; *y = event.y;
        result = 1;
    }
    pthread_mutex_unlock(&lock);
    return result;
}

static int get_key(gnutls_session_t session, const char *identity, gnutls_datum_t *key)
{
    (void)session;
    if (strcmp(identity, "wowps5-input")) return -1;
    key->data = gnutls_malloc(sizeof(secret));
    if (!key->data) return -1;
    memcpy(key->data, secret, sizeof(secret));
    key->size = sizeof(secret);
    return 0;
}

static ssize_t pull(gnutls_transport_ptr_t pointer, void *data, size_t size)
{ return recv((int)(intptr_t)pointer, data, size, 0); }
static ssize_t push(gnutls_transport_ptr_t pointer, const void *data, size_t size)
{ return send((int)(intptr_t)pointer, data, size, 0); }

static int receive(gnutls_session_t session, void *data, size_t size)
{
    unsigned char *out = data;
    while (size)
    {
        ssize_t got = gnutls_record_recv(session, out, size);
        if (got == GNUTLS_E_INTERRUPTED) continue;
        if (got <= 0) return -1;
        out += got; size -= got;
    }
    return 0;
}

static void *input_thread(void *argument)
{
    int listener = (int)(intptr_t)argument;
    gnutls_psk_server_credentials_t credentials;
    if (gnutls_psk_allocate_server_credentials(&credentials) < 0) { close(listener); return NULL; }
    gnutls_psk_set_server_credentials_function(credentials, get_key);
    for (;;)
    {
        int fd = accept(listener, NULL, NULL), result;
        gnutls_session_t session;
        if (fd < 0) { if (errno == EINTR) continue; break; }
        struct timeval timeout = {15, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (gnutls_init(&session, GNUTLS_SERVER) < 0) { close(fd); continue; }
        /* TLS 1.3 PSK authentication. Both sides prove knowledge of the run key. */
        result = gnutls_priority_set_direct(session, "NORMAL:-VERS-ALL:+VERS-TLS1.3:+PSK:+DHE-PSK:+ECDHE-PSK", NULL);
        if (result >= 0) result = gnutls_credentials_set(session, GNUTLS_CRD_PSK, credentials);
        gnutls_transport_set_ptr(session, (void *)(intptr_t)fd);
        gnutls_transport_set_pull_function(session, pull);
        gnutls_transport_set_push_function(session, push);
        if (result >= 0) do { result = gnutls_handshake(session); } while (result == GNUTLS_E_INTERRUPTED);
        if (result < 0) fprintf(stderr, "[WoWPS5 input] TLS handshake rejected: %s\n", gnutls_strerror(result));
        if (result >= 0)
        {
            uint32_t wire[6];
            while (!receive(session, wire, sizeof(wire)))
            {
                struct event event = {ntohl(wire[1]), ntohl(wire[2]), ntohl(wire[3]),
                    (int32_t)ntohl(wire[4]), (int32_t)ntohl(wire[5])};
                unsigned char ack = 0;
                if (ntohl(wire[0]) == 0x57503549 &&
                    ((event.type == 1 && event.code <= 65535 && !(event.flags & ~6u)) ||
                     (event.type == 2 && !(event.flags & ~0xd9ffU))))
                {
                    ack = wowps5_input_queue(event.type, event.code, event.flags, event.x, event.y);
                }
                memset(wire, 0, sizeof(wire));
                if (gnutls_record_send(session, &ack, 1) != 1) break;
            }
        }
        gnutls_deinit(session);
        close(fd);
    }
    gnutls_psk_free_server_credentials(credentials);
    close(listener);
    return NULL;
}

void wowps5_input_start(void)
{
    extern void wowps5_devices_start(void);
    wowps5_devices_start();
    if (!getenv("WOWPS5_INPUT_TLS")) return;
    FILE *file = fopen("/app0/wine-input.key", "rb");
    if (!file) { fprintf(stderr, "[WoWPS5 input] missing run key; input disabled\n"); return; }
    size_t got = fread(secret, 1, sizeof(secret), file);
    int extra = fgetc(file);
    fclose(file);
    remove("/app0/wine-input.key");
    if (got != sizeof(secret) || extra != EOF || gnutls_global_init() < 0)
    { fprintf(stderr, "[WoWPS5 input] initialization failed; input disabled\n"); return; }
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in address = {.sin_len = sizeof(address), .sin_family = AF_INET,
        .sin_port = htons(37081), .sin_addr.s_addr = htonl(INADDR_ANY)};
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (fd < 0 || bind(fd, (struct sockaddr *)&address, sizeof(address)) || listen(fd, 2))
    { if (fd >= 0) close(fd); fprintf(stderr, "[WoWPS5 input] listener failed; input disabled\n"); return; }
    pthread_t thread;
    if (pthread_create(&thread, NULL, input_thread, (void *)(intptr_t)fd)) { close(fd); return; }
    pthread_detach(thread);
    fprintf(stderr, "[WoWPS5 input] authenticated TLS input ready\n");
}
#endif
