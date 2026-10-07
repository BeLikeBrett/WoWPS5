// Wine inside the title. MIT license.
// Wine's native side (build/ps5-wine/wine-unix.o) is linked into this title.
// A run is requested with a file in the title's folder: the first line is the
// Windows program's path on the console, the rest are NAME=value environment
// settings or, with a leading +, arguments for the program.
// __wine_main does not return: the title ends with the Windows program.
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#ifndef __PROSPERO__
void wowps5WineStartIfRequested(const char* requestPath) { (void)requestPath; }
bool wowps5WineWanted(const char* requestPath) { (void)requestPath; return false; }
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <unistd.h>
#include <sys/filio.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/types.h>

void __wine_main(int argc, char* argv[]);
void wowps5_input_start(void);

// A working directory for Wine. A title has none (evidence/wine-contract-v1:
// chdir is EPERM, and "." or any relative path is EINVAL), while Wine lists a
// directory by changing into it and opens files relative to directory handles.
// The directory is kept here as a path, and every path call Wine's native side
// makes is given an absolute path. Wine serialises its own use (dir_mutex).
#define PATH_BYTES 2048
static char workingDirectory[PATH_BYTES] = "/";
static pthread_mutex_t directoryLock = PTHREAD_MUTEX_INITIALIZER;
static char* handlePaths[4096];             // descriptor -> its directory's path, as Wine reported it (wowps5_remember_fd)

// path made absolute and tidied: no "." or empty components, ".." folded. Returns 0, or -1 with errno.
static int absolute(const char* directory, const char* path, char* out) {
    char joined[PATH_BYTES * 2]; size_t used = 0;
    if(!path || !*path) { errno = ENOENT; return -1; }
    if(snprintf(joined, sizeof(joined), "%s/%s", path[0] == '/' ? "" : directory, path) >= (int)sizeof(joined)) { errno = ENAMETOOLONG; return -1; }
    out[0] = 0;
    for(char* part = joined; *part;) {
        while(*part == '/') part++;
        char* end = part; while(*end && *end != '/') end++;
        const size_t length = (size_t)(end - part);
        if(length == 2 && part[0] == '.' && part[1] == '.') { while(used && out[--used] != '/') {} out[used] = 0; }
        else if(length && !(length == 1 && part[0] == '.')) {
            if(used + length + 2 > PATH_BYTES) { errno = ENAMETOOLONG; return -1; }
            out[used++] = '/'; memcpy(out + used, part, length); used += length; out[used] = 0;
        }
        part = end;
    }
    if(!used) { out[0] = '/'; out[1] = 0; }
    return 0;
}
static int resolve(const char* path, char* out) {
    char directory[PATH_BYTES];
    pthread_mutex_lock(&directoryLock); strcpy(directory, workingDirectory); pthread_mutex_unlock(&directoryLock);
    return absolute(directory, path, out);
}
// relative to a descriptor: the path Wine gave for it, or the working directory for AT_FDCWD
static int resolveAt(int fd, const char* path, char* out) {
    if(fd == AT_FDCWD || (path && path[0] == '/')) return resolve(path, out);
    char directory[PATH_BYTES] = "";
    pthread_mutex_lock(&directoryLock);
    if(fd >= 0 && fd < 4096 && handlePaths[fd]) snprintf(directory, sizeof(directory), "%s", handlePaths[fd]);
    pthread_mutex_unlock(&directoryLock);
    if(!directory[0]) { errno = EBADF; return -1; }
    return absolute(directory, path, out);
}
void wowps5_remember_fd(int fd, const char* path) {
    if(fd < 0 || fd >= 4096) return;
    pthread_mutex_lock(&directoryLock);
    free(handlePaths[fd]); handlePaths[fd] = path ? strdup(path) : NULL;
    pthread_mutex_unlock(&directoryLock);
}
int wowps5_chdir(const char* path) {
    char full[PATH_BYTES]; struct stat status;
    if(resolve(path, full) || stat(full, &status)) return -1;
    if(!S_ISDIR(status.st_mode)) { errno = ENOTDIR; return -1; }
    pthread_mutex_lock(&directoryLock); strcpy(workingDirectory, full); pthread_mutex_unlock(&directoryLock);
    return 0;
}
int wowps5_fchdir(int fd) {
    char full[PATH_BYTES];
    if(resolveAt(fd, ".", full)) { errno = ENOSYS; return -1; }     // a descriptor Wine gave no path for
    return wowps5_chdir(full);
}
char* wowps5_getcwd(char* buffer, size_t bytes) {
    pthread_mutex_lock(&directoryLock);
    const size_t needed = strlen(workingDirectory) + 1;
    if(!buffer) buffer = malloc(bytes > needed ? bytes : needed), bytes = bytes > needed ? bytes : needed;
    if(buffer && bytes >= needed) memcpy(buffer, workingDirectory, needed); else { if(buffer) errno = ERANGE; buffer = NULL; }
    pthread_mutex_unlock(&directoryLock);
    return buffer;
}
// libc's realpath walks the path with lstat, which is refused: the tidy absolute path of something that exists
char* wowps5_realpath(const char* path, char* buffer) {
    char full[PATH_BYTES]; struct stat status;
    if(resolve(path, full) || stat(full, &status)) return NULL;
    if(!buffer) return strdup(full);
    strcpy(buffer, full); return buffer;
}
DIR* ps5_opendir(const char* path);
int ps5_openat(int directory, const char* name, int flags, ...);
int ps5_fstatat(int directory, const char* name, struct stat* result, int flags);
int wowps5_open(const char* path, int flags, ...) {
    char full[PATH_BYTES]; int mode = 0;
    if(flags & O_CREAT) { va_list arguments; va_start(arguments, flags); mode = va_arg(arguments, int); va_end(arguments); }
    return resolve(path, full) ? -1 : open(full, flags, mode);
}
int wowps5_openat(int fd, const char* path, int flags, ...) {
    char full[PATH_BYTES]; int mode = 0;
    if(flags & O_CREAT) { va_list arguments; va_start(arguments, flags); mode = va_arg(arguments, int); va_end(arguments); }
    return resolveAt(fd, path, full) ? -1 : ps5_openat(AT_FDCWD, full, flags, mode);
}
bool wowps5_flush_same_file(const struct stat* info);
int wowps5_fstatat(int fd, const char* path, struct stat* result, int flags) {
    char full[PATH_BYTES];
    if(resolveAt(fd, path, full)) return -1;
    int status = stat(full, result);                              // nothing a title can name is a symbolic link it made
    if(!status && wowps5_flush_same_file(result)) status = stat(full, result);
    return status;
}
int wowps5_stat(const char* path, struct stat* result) {
    char full[PATH_BYTES];
    if(resolve(path, full)) return -1;
    int status = stat(full, result);
    if(!status && wowps5_flush_same_file(result)) status = stat(full, result);    // its size as the program left it
    return status;
}
// lstat is refused with EPERM for every path; stat works, and a title cannot make a symbolic link.
int wowps5_lstat(const char* path, struct stat* result) { return wowps5_stat(path, result); }
int wowps5_access(const char* path, int mode) { char full[PATH_BYTES]; return resolve(path, full) ? -1 : access(full, mode); }
int wowps5_mkdir(const char* path, mode_t mode) { char full[PATH_BYTES]; return resolve(path, full) ? -1 : mkdir(full, mode); }
int wowps5_rmdir(const char* path) { char full[PATH_BYTES]; return resolve(path, full) ? -1 : rmdir(full); }
int wowps5_unlink(const char* path) { char full[PATH_BYTES]; return resolve(path, full) ? -1 : unlink(full); }
int wowps5_rename(const char* from, const char* to) {
    char source[PATH_BYTES], target[PATH_BYTES];
    return resolve(from, source) || resolve(to, target) ? -1 : rename(source, target);
}
DIR* wowps5_opendir(const char* path) { char full[PATH_BYTES]; return resolve(path, full) ? NULL : ps5_opendir(full); }
FILE* wowps5_fopen(const char* path, const char* mode) { char full[PATH_BYTES]; return resolve(path, full) ? NULL : fopen(full, mode); }

// The platform layer answers fstatfs itself (the kernel refuses it to a title) and
// leaves the flags empty; Wine reads a volume without MNT_LOCAL as a network drive.
int ps5_fstatfs(int fd, void* result);
int wowps5_fstatfs(int fd, struct statfs* result) {
    const int status = ps5_fstatfs(fd, result);
    if(!status) result->f_flags |= MNT_LOCAL;
    return status;
}

// h_errno's location: no system module exports libc's.
int* wowps5___h_errno(void) { static _Thread_local int value; return &value; }

// No system module exports gethostbyaddr: reverse lookups report that no answer can be had.
struct hostent* wowps5_gethostbyaddr(const void* address, socklen_t length, int family) {
    (void)address; (void)length; (void)family; *wowps5___h_errno() = NO_RECOVERY; return NULL;
}

// Name resolution. The platform layer's getaddrinfo only reports failure, and
// libc's is not exported; the console's own resolver (libSceNet) answers for
// IPv4 names. Numeric addresses, "localhost" and a missing node need no lookup.
int sceNetPoolCreate(const char* name, int bytes, int flags);
int sceNetPoolDestroy(int pool);
int sceNetResolverCreate(const char* name, int pool, int flags);
int sceNetResolverStartNtoa(int resolver, const char* host, struct in_addr* address, int timeout, int retries, int flags);
int sceNetResolverDestroy(int resolver);
void wowps5_freeaddrinfo(struct addrinfo* info) {
    while(info) { struct addrinfo* next = info->ai_next; free(info); info = next; }
}
int wowps5_getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** result) {
    struct in_addr address; unsigned port = 0;
    const int family = hints ? hints->ai_family : AF_UNSPEC, flags = hints ? hints->ai_flags : 0;
    if(result) *result = NULL;
    if(!result || (!node && !service)) return EAI_NONAME;
    if(family != AF_UNSPEC && family != AF_INET) return EAI_FAMILY;       // no IPv6 answers here
    if(service && *service) {
        char* end; const long number = strtol(service, &end, 10);
        if(!*end && number >= 0 && number < 65536) port = (unsigned)number;
        else if(flags & AI_NUMERICSERV) return EAI_NONAME;
        else if(!strcmp(service, "http")) port = 80;
        else if(!strcmp(service, "https")) port = 443;
        else return EAI_SERVICE;
    }
    if(!node) address.s_addr = htonl(flags & AI_PASSIVE ? INADDR_ANY : INADDR_LOOPBACK);
    else if(inet_pton(AF_INET, node, &address) == 1) {}
    else if(flags & AI_NUMERICHOST) return EAI_NONAME;
    else if(!strcasecmp(node, "localhost")) address.s_addr = htonl(INADDR_LOOPBACK);
    else {
        static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
        static int pool = -1;
        pthread_mutex_lock(&lock);
        if(pool < 0) pool = sceNetPoolCreate("wowps5 resolver", 64 * 1024, 0);
        pthread_mutex_unlock(&lock);
        const int resolver = pool < 0 ? -1 : sceNetResolverCreate("wowps5", pool, 0);
        if(resolver < 0) return EAI_FAIL;
        const int status = sceNetResolverStartNtoa(resolver, node, &address, 5 * 1000 * 1000, 2, 0);
        sceNetResolverDestroy(resolver);
        if(status < 0) return EAI_NONAME;
    }
    // one answer for each socket type asked for (both when none was)
    static const int types[2][2] = { { SOCK_STREAM, IPPROTO_TCP }, { SOCK_DGRAM, IPPROTO_UDP } };
    struct addrinfo** next = result;
    for(int i = 0; i < 2; i++) {
        if(hints && hints->ai_socktype && hints->ai_socktype != types[i][0]) continue;
        struct { struct addrinfo info; struct sockaddr_in in; }* entry = calloc(1, sizeof(*entry));
        if(!entry) { wowps5_freeaddrinfo(*result); *result = NULL; return EAI_MEMORY; }
        entry->in.sin_len = sizeof(entry->in); entry->in.sin_family = AF_INET; entry->in.sin_port = htons((unsigned short)port); entry->in.sin_addr = address;
        entry->info.ai_family = AF_INET; entry->info.ai_socktype = types[i][0];
        entry->info.ai_protocol = hints && hints->ai_protocol ? hints->ai_protocol : types[i][1];
        entry->info.ai_addrlen = sizeof(entry->in); entry->info.ai_addr = (struct sockaddr*)&entry->in;
        *next = &entry->info; next = &entry->info.ai_next;
    }
    return *result ? 0 : EAI_SOCKTYPE;
}
// libkernel's gethostname gives an empty name to a title.
int wowps5_gethostname(char* name, size_t bytes) {
    if(gethostname(name, bytes) || !name[0]) snprintf(name, bytes, "ps5");
    return 0;
}

// Null on this firmware (the import audit): gethostbyname and getnameinfo.
// The first is built on getaddrinfo, which the platform layer supplies; the
// second gives the numeric form, and no name where a name was required.
struct hostent* wowps5_gethostbyname(const char* name) {
    static _Thread_local struct hostent entry;
    static _Thread_local struct in_addr addresses[8];
    static _Thread_local char* list[9]; static _Thread_local char* none[1]; static _Thread_local char canonical[256];
    struct addrinfo hints, *found = NULL, *each; int count = 0;
    memset(&hints, 0, sizeof(hints)); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if(wowps5_getaddrinfo(name, NULL, &hints, &found) || !found) { *wowps5___h_errno() = HOST_NOT_FOUND; return NULL; }
    for(each = found; each && count < 8; each = each->ai_next)
        if(each->ai_family == AF_INET) { addresses[count] = ((struct sockaddr_in*)each->ai_addr)->sin_addr; list[count] = (char*)&addresses[count]; count++; }
    wowps5_freeaddrinfo(found);
    if(!count) { *wowps5___h_errno() = NO_DATA; return NULL; }
    list[count] = NULL; none[0] = NULL;
    snprintf(canonical, sizeof(canonical), "%s", name);
    entry.h_name = canonical; entry.h_aliases = none; entry.h_addrtype = AF_INET; entry.h_length = sizeof(struct in_addr); entry.h_addr_list = list;
    return &entry;
}
int wowps5_getnameinfo(const struct sockaddr* address, socklen_t length, char* host, size_t hostBytes, char* service, size_t serviceBytes, int flags) {
    (void)length;
    if(address->sa_family != AF_INET && address->sa_family != AF_INET6) return EAI_FAMILY;
    const void* raw = address->sa_family == AF_INET ? (const void*)&((const struct sockaddr_in*)address)->sin_addr
                                                    : (const void*)&((const struct sockaddr_in6*)address)->sin6_addr;
    const unsigned port = ntohs(address->sa_family == AF_INET ? ((const struct sockaddr_in*)address)->sin_port : ((const struct sockaddr_in6*)address)->sin6_port);
    if(host && hostBytes) {
        if(flags & NI_NAMEREQD) return EAI_NONAME;          // no reverse lookup to ask
        if(!inet_ntop(address->sa_family, raw, host, (socklen_t)hostBytes)) return EAI_OVERFLOW;
    }
    if(service && serviceBytes && snprintf(service, serviceBytes, "%u", port) >= (int)serviceBytes) return EAI_OVERFLOW;
    return 0;
}

// What no system module gives a title. Each reports the failure the caller's
// own error path handles; none pretends to have worked.
// Null on this firmware (the audit in wine_contract_probe.c): only libkernel_sys has them.
int wowps5_getfsstat(struct statfs* buffer, long bytes, int mode) { (void)buffer; (void)bytes; (void)mode; return 0; }   // no mount list
int wowps5_isatty(int fd) { (void)fd; errno = ENOTTY; return 0; }
int wowps5_getmntinfo(struct statfs** buffer, int mode) { (void)buffer; (void)mode; return 0; }   // no mount list: no extra DOS devices
speed_t wowps5_cfgetospeed(const struct termios* settings) { (void)settings; return 0; }
int wowps5_cfsetispeed(struct termios* settings, speed_t speed) { (void)settings; (void)speed; errno = ENOTTY; return -1; }
int wowps5_cfsetospeed(struct termios* settings, speed_t speed) { (void)settings; (void)speed; errno = ENOTTY; return -1; }
ssize_t wowps5_extattr_get_fd(int fd, int space, const char* name, void* data, size_t bytes) { (void)fd; (void)space; (void)name; (void)data; (void)bytes; errno = EOPNOTSUPP; return -1; }
ssize_t wowps5_extattr_get_file(const char* path, int space, const char* name, void* data, size_t bytes) { (void)path; (void)space; (void)name; (void)data; (void)bytes; errno = EOPNOTSUPP; return -1; }
ssize_t wowps5_extattr_set_fd(int fd, int space, const char* name, const void* data, size_t bytes) { (void)fd; (void)space; (void)name; (void)data; (void)bytes; errno = EOPNOTSUPP; return -1; }
int wowps5_extattr_delete_fd(int fd, int space, const char* name) { (void)fd; (void)space; (void)name; errno = EOPNOTSUPP; return -1; }
int wowps5_thr_set_name(long id, const char* name) { (void)id; (void)name; errno = ENOSYS; return -1; }
// dup, dup2 and fcntl(F_DUPFD) are all refused to a title on this firmware
// (evidence/wine-contract-v1: EPERM and EINVAL for every kind of descriptor),
// while a descriptor passed over a Unix socket arrives as a new one that
// shares the file position, which is what dup gives. One socket pair, one
// descriptor in flight at a time.
// The monotonic clock. clock_gettime(CLOCK_MONOTONIC) costs 20 microseconds on
// this console (the real-time clock 0.9), and Wine reads it for every
// QueryPerformanceCounter, timed wait and server pass. The processor's time
// stamp counter, which the console offers games as their timer, gives the same
// clock for the cost of an instruction: anchored once to the kernel's value.
uint64_t sceKernelGetTscFrequency(void);
static uint64_t clockFrequency, clockBaseTsc, clockBaseNs;
static pthread_once_t clockOnce = PTHREAD_ONCE_INIT;
static void clockStart(void) {
    struct timespec base;
    const uint64_t frequency = sceKernelGetTscFrequency();
    if(clock_gettime(CLOCK_MONOTONIC, &base) != 0 || frequency < 500000000ull || frequency > 10000000000ull) return;
    clockBaseTsc = __builtin_ia32_rdtsc();
    clockBaseNs = (uint64_t)base.tv_sec * 1000000000ull + (uint64_t)base.tv_nsec;
    clockFrequency = frequency;
}
int wowps5_clock_gettime(clockid_t id, struct timespec* out) {
    if(id != CLOCK_MONOTONIC && id != CLOCK_UPTIME && id != CLOCK_MONOTONIC_PRECISE && id != CLOCK_MONOTONIC_FAST &&
       id != CLOCK_UPTIME_PRECISE && id != CLOCK_UPTIME_FAST) return clock_gettime(id, out);
    pthread_once(&clockOnce, clockStart);
    if(!clockFrequency) return clock_gettime(id, out);
    const uint64_t ticks = __builtin_ia32_rdtsc() - clockBaseTsc;
    const uint64_t ns = clockBaseNs + (uint64_t)((unsigned __int128)ticks * 1000000000ull / clockFrequency);
    out->tv_sec = (time_t)(ns / 1000000000ull); out->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}

// Writes. Every write to the console's storage costs 0.7 ms whatever its size,
// and one that extends the file 4.3 ms; a Windows program appends log lines and
// cache records a few bytes at a time on the threads that draw. Small writes at
// a file's position are therefore kept here and written together: when 64 KiB
// have gathered, a quarter of a second after the first, or before anything else
// is done with that descriptor (read, seek, size, map, close). What is lost if
// the title dies is at most that quarter second of a log.
#define HELD_FDS 4096
#define HELD_BYTES 65536
#define HELD_LOCKS 64
struct held { char* data; _Atomic uint32_t used; _Atomic uint8_t kind; uint64_t since; dev_t device; ino_t inode; };   // kind 0 unknown, 1 a regular file, 2 not one
static struct held held[HELD_FDS];
// One lock for every 64th descriptor: a write-out takes milliseconds and must not
// hold up the pipes and sockets that share the table.
static pthread_mutex_t heldLocks[HELD_LOCKS] = { [0 ... HELD_LOCKS - 1] = PTHREAD_MUTEX_INITIALIZER };
static atomic_uint heldCount;            // descriptors with something held
static uint64_t heldNow(void) { struct timespec t; wowps5_clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static void heldWriteOut(int fd) {       // with the descriptor's lock
    struct held* h = &held[fd];
    const uint32_t used = h->used;
    for(uint32_t done = 0; done < used; ) {
        const ssize_t wrote = write(fd, h->data + done, used - done);
        if(wrote <= 0) break;            // the program was told its write succeeded: nothing better to do than drop it
        done += (uint32_t)wrote;
    }
    if(used) { h->used = 0; atomic_fetch_sub(&heldCount, 1); }
}
void wowps5_flush_fd(int fd) {
    if(fd < 0 || fd >= HELD_FDS || !held[fd].used) return;
    pthread_mutex_lock(&heldLocks[fd % HELD_LOCKS]); heldWriteOut(fd); pthread_mutex_unlock(&heldLocks[fd % HELD_LOCKS]);
}
void wowps5_flush_writes(void) {
    for(int fd = 0; atomic_load(&heldCount) && fd < HELD_FDS; fd++) wowps5_flush_fd(fd);
}
// A file has more descriptors than the one that was written to: Wine's server
// has its own, a mapping reads through another, and a name opens a new one.
// Whatever looks at a file through one of those first has what is held for the
// same file written out. Costs an fstat, and only while something is held.
static atomic_int heldHighest;
static bool heldFlushFile(const struct stat* info) {
    bool flushed = false;
    if(!S_ISREG(info->st_mode)) return false;
    const int highest = atomic_load(&heldHighest);
    for(int fd = 3; fd <= highest && fd < HELD_FDS; fd++)
        if(held[fd].used && held[fd].device == info->st_dev && held[fd].inode == info->st_ino) { wowps5_flush_fd(fd); flushed = true; }
    return flushed;
}
// For a file looked at by name: true when something was written out, and the caller looks again.
bool wowps5_flush_same_file(const struct stat* info) { return atomic_load(&heldCount) && heldFlushFile(info); }
static void heldSeen(int fd) {
    wowps5_flush_fd(fd);
    struct stat info;
    if(atomic_load(&heldCount) && fstat(fd, &info) == 0) heldFlushFile(&info);
}
static void* heldThread(void* unused) {
    (void)unused;
    for(;;) {
        usleep(100000);
        if(!atomic_load(&heldCount)) continue;
        const uint64_t now = heldNow();
        for(int fd = 0; fd < HELD_FDS; fd++) {
            if(!held[fd].used) continue;
            pthread_mutex_lock(&heldLocks[fd % HELD_LOCKS]);
            if(held[fd].used && now - held[fd].since >= 250) heldWriteOut(fd);
            pthread_mutex_unlock(&heldLocks[fd % HELD_LOCKS]);
        }
    }
    return NULL;
}
// A file that has been mapped is written straight through from then on: a view
// shows the file, not what is held here for it.
static struct { dev_t device; ino_t inode; } heldMapped[512];
static atomic_uint heldMappedCount;
static bool heldIsMapped(const struct stat* info) {
    const unsigned count = atomic_load(&heldMappedCount);
    for(unsigned i = 0; i < count && i < 512; i++) if(heldMapped[i].device == info->st_dev && heldMapped[i].inode == info->st_ino) return true;
    return false;
}
static void heldStart(void) { pthread_t thread; if(!pthread_create(&thread, NULL, heldThread, NULL)) pthread_detach(thread); }
static bool heldOff;
ssize_t wowps5_write(int fd, const void* data, size_t bytes) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    if(heldOff || fd < 3 || fd >= HELD_FDS || (held[fd].kind == 2 && !held[fd].used)) return write(fd, data, bytes);
    if(bytes == 0 || bytes > HELD_BYTES / 2) { wowps5_flush_fd(fd); return write(fd, data, bytes); }
    pthread_mutex_t* lock = &heldLocks[fd % HELD_LOCKS];
    pthread_mutex_lock(lock);
    struct held* h = &held[fd];
    if(!h->used) {                       // what the number names is looked at again each time a batch begins
        struct stat info;
        h->kind = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && !heldIsMapped(&info) ? 1 : 2;
        h->device = info.st_dev; h->inode = info.st_ino;
        int highest = atomic_load(&heldHighest);
        while(fd > highest && !atomic_compare_exchange_weak(&heldHighest, &highest, fd)) {}
    }
    if(h->kind != 1 || (!h->data && !(h->data = malloc(HELD_BYTES)))) {
        pthread_mutex_unlock(lock);
        return write(fd, data, bytes);
    }
    if(h->used + bytes > HELD_BYTES) heldWriteOut(fd);
    if(!h->used) { h->since = heldNow(); atomic_fetch_add(&heldCount, 1); }
    memcpy(h->data + h->used, data, bytes); h->used += (uint32_t)bytes;
    pthread_mutex_unlock(lock);
    pthread_once(&once, heldStart);
    return (ssize_t)bytes;
}
// Everything else done with a descriptor sees the file as the program left it.
ssize_t wowps5_read(int fd, void* data, size_t bytes) { heldSeen(fd); return read(fd, data, bytes); }
// WOWPS5_IO_STATS=1: what the program reads from files, said every ten seconds:
// how many reads, how much, how long they took, and how many began where (or
// within 256 KiB after) the read before on the same descriptor ended, which is
// what reading ahead would save.
static bool ioStats;
static atomic_ullong ioReads, ioBytes, ioNanoseconds, ioSlow, ioNear, ioSmall;
static _Atomic off_t ioEnd[HELD_FDS];
static uint64_t ioNow(void) { struct timespec t; wowps5_clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
ssize_t wowps5_pread(int fd, void* data, size_t bytes, off_t offset) {
    heldSeen(fd);
    if(!ioStats || fd < 0 || fd >= HELD_FDS) return pread(fd, data, bytes, offset);
    const uint64_t begin = ioNow();
    const ssize_t got = pread(fd, data, bytes, offset);
    const uint64_t took = ioNow() - begin;
    const off_t before = atomic_exchange(&ioEnd[fd], offset + (got > 0 ? got : 0));
    atomic_fetch_add(&ioReads, 1); atomic_fetch_add(&ioBytes, got > 0 ? (unsigned long long)got : 0); atomic_fetch_add(&ioNanoseconds, took);
    if(took > 200000) atomic_fetch_add(&ioSlow, 1);
    if(offset >= before && offset < before + (256 << 10)) atomic_fetch_add(&ioNear, 1);
    if(bytes < 16384) atomic_fetch_add(&ioSmall, 1);
    return got;
}
ssize_t wowps5_pwrite(int fd, const void* data, size_t bytes, off_t offset) { heldSeen(fd); return pwrite(fd, data, bytes, offset); }
off_t wowps5_lseek(int fd, off_t offset, int whence) { heldSeen(fd); return lseek(fd, offset, whence); }
int wowps5_fstat(int fd, struct stat* info) { heldSeen(fd); return fstat(fd, info); }
int wowps5_ftruncate(int fd, off_t length) { heldSeen(fd); return ftruncate(fd, length); }
int wowps5_fsync(int fd) { heldSeen(fd); return fsync(fd); }
void* wowps5_mmap(void* address, size_t bytes, int prot, int flags, int fd, off_t offset) {
    struct stat info;
    if(fd >= 0 && fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && !heldIsMapped(&info)) {
        static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&lock);
        const unsigned count = atomic_load(&heldMappedCount);
        if(count < 512) { heldMapped[count].device = info.st_dev; heldMapped[count].inode = info.st_ino; atomic_store(&heldMappedCount, count + 1); }
        pthread_mutex_unlock(&lock);
        wowps5_flush_writes();           // whichever descriptor of the file holds something
    }
    return mmap(address, bytes, prot, flags, fd, offset);
}
int wowps5_close(int fd) {
    if(fd >= 0 && fd < HELD_FDS && (held[fd].used || held[fd].kind)) {
        pthread_mutex_lock(&heldLocks[fd % HELD_LOCKS]);
        heldWriteOut(fd);
        held[fd].kind = 0;               // the number will name another file
        pthread_mutex_unlock(&heldLocks[fd % HELD_LOCKS]);
    }
    return close(fd);
}

int wowps5_dup(int fd) {
    wowps5_flush_fd(fd);
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static int pair[2] = { -1, -1 };
    char byte = 'D', control[CMSG_SPACE(sizeof(int))];
    struct iovec vector = { &byte, 1 };
    struct msghdr message;
    int copy = -1, error = 0;
    pthread_mutex_lock(&lock);
    if(pair[0] < 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) { error = errno; pair[0] = pair[1] = -1; }
    if(!error) {
        memset(&message, 0, sizeof(message)); memset(control, 0, sizeof(control));
        message.msg_iov = &vector; message.msg_iovlen = 1; message.msg_control = control; message.msg_controllen = sizeof(control);
        struct cmsghdr* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS; header->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header), &fd, sizeof(int));
        ssize_t moved;
        while((moved = sendmsg(pair[0], &message, 0)) < 0 && errno == EINTR) {}
        if(moved != 1) error = errno ? errno : EIO;      // EBADF for a descriptor that is not open, as dup reports
        else {
            memset(control, 0, sizeof(control)); message.msg_controllen = sizeof(control);
            while((moved = recvmsg(pair[1], &message, 0)) < 0 && errno == EINTR) {}
            header = CMSG_FIRSTHDR(&message);
            if(moved == 1 && header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) memcpy(&copy, CMSG_DATA(header), sizeof(int));
            else error = moved < 0 ? errno : EMFILE;
        }
    }
    pthread_mutex_unlock(&lock);
    if(copy < 0) errno = error;
    return copy;
}
// fcntl as this firmware gives it to a title (evidence/wine-contract-v1):
// F_SETFL on a socket reports EACCES although the flag is set; F_GETFD and
// F_SETFD are EINVAL; F_DUPFD is EINVAL.
int wowps5_fcntl(int fd, int command, ...) {
    va_list arguments; va_start(arguments, command);
    const intptr_t argument = va_arg(arguments, intptr_t);
    va_end(arguments);
    switch(command) {
    case F_SETFL: {
        // On a socket this reports EACCES and sets the flag bit without the socket
        // ceasing to block; only Sony's SO_NBIO option changes that (measured:
        // a receive after F_SETFL, FIONBIO or SOCK_NONBLOCK still waits).
        const int result = fcntl(fd, F_SETFL, (int)argument);
        if(result == -1 && errno == EACCES) {
            int enable = ((int)argument & O_NONBLOCK) != 0;
            return setsockopt(fd, SOL_SOCKET, 0x1200 /* SO_NBIO */, &enable, sizeof(enable));
        }
        return result;
    }
    case F_GETFD: return 0;      // close-on-exec has no meaning in a process that cannot exec
    case F_SETFD: return 0;
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: return wowps5_dup(fd);   // the lowest free number, whatever minimum was asked
    default: return fcntl(fd, command, argument);
    }
}
// FIONBIO on a socket is refused the same way; other requests pass through.
int wowps5_ioctl(int fd, unsigned long request, ...) {
    va_list arguments; va_start(arguments, request);
    void* argument = va_arg(arguments, void*);
    va_end(arguments);
    const int result = ioctl(fd, request, argument);
    if(result == -1 && errno == EACCES && request == FIONBIO && argument) {
        int enable = *(int*)argument != 0;
        return setsockopt(fd, SOL_SOCKET, 0x1200 /* SO_NBIO */, &enable, sizeof(enable));
    }
    return result;
}
int wowps5_pipe2(int fds[2], int flags) {
    if(pipe(fds)) return -1;
    for(int i = 0; i < 2; i++) {
        if(flags & O_CLOEXEC) fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        if(flags & O_NONBLOCK) fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
    }
    return 0;
}

// Wine's own diagnostics, straight to klog a line at a time: the stderr stream
// goes through a pipe and a reader thread, which a dying process never drains.
int sceKernelDebugOutText(int channel, const char* text);
ssize_t wowps5_debug_write(const void* text, size_t length) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static char line[1024]; static size_t used;
    static const char prefix[] = "[WoWPS5 Probe] ";
    const char* bytes = text;
    pthread_mutex_lock(&lock);
    for(size_t i = 0; i < length; i++) {
        if(!used) { memcpy(line, prefix, sizeof(prefix) - 1); used = sizeof(prefix) - 1; }
        line[used++] = bytes[i];
        if(bytes[i] == '\n' || used == sizeof(line) - 2) {
            if(bytes[i] != '\n') line[used++] = '\n';
            line[used] = 0;
            sceKernelDebugOutText(0, line);
            used = 0;
        }
    }
    pthread_mutex_unlock(&lock);
    return (ssize_t)length;
}

// exit() and _exit() kill a title, and the console records a crash over a run
// that worked. The Windows program's exit goes the way the title's own does:
// the shell is asked to close it. Other threads run until the shell does.
void catchReturnFromMain(int status);
void ps5w_flush(void);   // runtime/wine-ps5/ps5_mman.h
void wowps5_osk_shutdown(void);   // app/runtime/wine_osk.c: an open on-screen keyboard is dismissed, without waiting
void wowps5_exit(int status) {
    char line[96];
    snprintf(line, sizeof(line), "[WoWPS5 Probe] [WoWPS5 Wine] the Windows program ended with status %d\n", status);
    sceKernelDebugOutText(0, line);
    wowps5_osk_shutdown();
    wowps5_flush_writes();
    ps5w_flush();
    catchReturnFromMain(status);
    // The shell closes the title when asked. Should it not, the process ends
    // itself: the console logs that as a crash, and is rid of the title.
    sleep(20);
    sceKernelDebugOutText(0, "[WoWPS5 Probe] [WoWPS5 Wine] the shell did not close the title: ending the process\n");
    _exit(status ? status : 1);
    for(;;) pause();
}

// A run is given a time limit (WOWPS5_DEADLINE, seconds, in the run request).
// A Windows program that hangs, or dies leaving threads behind, otherwise stays
// a title only the shell can close, and the shell has frozen over one that
// would not go (2026-10-04: its main thread stuck for the rest of the session,
// then the console's storage with it).
static void* deadlineThread(void* argument) {
    const unsigned seconds = (unsigned)(uintptr_t)argument;
    for(unsigned left = seconds; left; ) left = sleep(left);
    char line[128];
    snprintf(line, sizeof(line), "[WoWPS5 Probe] [WoWPS5 Wine] the run's %u s are over: the Windows program ended with status 124\n", seconds);
    sceKernelDebugOutText(0, line);
    wowps5_osk_shutdown();
    wowps5_flush_writes();
    ps5w_flush();
    catchReturnFromMain(124);
    sleep(15);
    sceKernelDebugOutText(0, "[WoWPS5 Probe] [WoWPS5 Wine] the shell did not close the title: ending the process\n");
    _exit(124);
    return NULL;
}

// What the title runs when it is started from the console's home screen, with
// no run request: the game, if it is installed, with the settings it is known
// to run with. /data/wowps5/launch.txt, when there is one, adds to or replaces
// them (the lines of a run request: NAME=value, +argument) and stays in place.
#define GAME_PROGRAM "/data/wowps5/prefix/dosdevices/c:/Games/World of Warcraft/_classic_beta_/WowB.exe"
#define LAUNCH_SETTINGS "/data/wowps5/launch.txt"

// A queued run of the title's own probe samples (/app0/test-run.txt, written by
// ps5/tools/run.sh) comes before the game.
#define PROBE_RUN "/app0/test-run.txt"
bool wowps5WineWanted(const char* requestPath) {
    return access(requestPath, R_OK) == 0 || (access(PROBE_RUN, R_OK) != 0 && access(GAME_PROGRAM, R_OK) == 0);
}

// The console's splash stays over the title until the Windows program has
// presented its first frames (win32u calls this), so that the start is the
// splash and then the game, with no black screen between them; and it goes
// after half a minute whatever happens, since a program may never present.
int sceSystemServiceHideSplashScreen(void);
void wowps5_first_frame(void) {
    static atomic_flag hidden = ATOMIC_FLAG_INIT;
    if(!atomic_flag_test_and_set(&hidden)) sceSystemServiceHideSplashScreen();
}
// What the title's descriptors are, said when their number has moved by 64 and
// when few are left. Running out shows only as files the game cannot open or
// rename (its saved variables, 2026-10-05), so the count is kept in the log.
static int descriptorsLeft(void) {
    static int spare[400]; int left = 0;
    while(left < 400 && (spare[left] = open("/dev/null", O_RDONLY)) >= 0) left++;
    for(int i = 0; i < left; i++) close(spare[i]);
    return left;
}
static void* descriptorThread(void* unused) {
    (void)unused;
    struct rlimit limit = {0};
    const int had = getrlimit(RLIMIT_NOFILE, &limit);
    const unsigned long long soft = (unsigned long long)limit.rlim_cur, hard = (unsigned long long)limit.rlim_max;
    if(!had && limit.rlim_cur < limit.rlim_max) { limit.rlim_cur = limit.rlim_max; if(setrlimit(RLIMIT_NOFILE, &limit)) limit.rlim_cur = (rlim_t)soft; }
    fprintf(stderr, "[WoWPS5 Wine] descriptors: limit %llu of at most %llu (getrlimit=%d), now %llu\n", soft, hard, had, (unsigned long long)limit.rlim_cur);
    const unsigned most = limit.rlim_cur > 0 && limit.rlim_cur < 32768 ? (unsigned)limit.rlim_cur : 32768;
    // Anonymous memory objects look like files and take none of the 250 places: known by their device.
    dev_t memoryDevice = 0; { const int object = shm_open(SHM_ANON, O_RDWR, 0600); struct stat info; if(object >= 0) { if(!fstat(object, &info)) memoryDevice = info.st_dev; close(object); } }
    unsigned said = 0;
    for(;;) {
        unsigned memory = 0, memoryKinds = 0;
        unsigned files = 0, pipes = 0, sockets = 0, other = 0, highest = 0, unlinked = 0, kinds = 0;
        static struct { dev_t device; ino_t inode; off_t bytes; unsigned count; } seen[4096];   // one entry a file, however many descriptors
        for(unsigned fd = 0; fd < most; fd++) {
            struct stat info;
            if(fstat((int)fd, &info)) { if(errno == EBADF) continue; other++; highest = fd; continue; }
            highest = fd;
            if(S_ISREG(info.st_mode) || S_ISDIR(info.st_mode)) {
                files++;
                if(!info.st_nlink) unlinked++;       // a section's file: the server removes its name at once
                unsigned at = 0;
                while(at < kinds && (seen[at].device != info.st_dev || seen[at].inode != info.st_ino)) at++;
                if(memoryDevice && info.st_dev == memoryDevice) memory++;
                if(at == kinds && memoryDevice && info.st_dev == memoryDevice) memoryKinds++;
                if(at == kinds && kinds < 4096) { seen[kinds].device = info.st_dev; seen[kinds].inode = info.st_ino; seen[kinds].bytes = info.st_size; seen[kinds++].count = 0; }
                if(at < 4096) seen[at].count++;
            }
            else if(S_ISFIFO(info.st_mode)) pipes++;
            else if(S_ISSOCK(info.st_mode)) sockets++;
            else other++;
        }
        if(ioStats) {
            const unsigned long long reads = atomic_exchange(&ioReads, 0), amount = atomic_exchange(&ioBytes, 0), spent = atomic_exchange(&ioNanoseconds, 0),
                slow = atomic_exchange(&ioSlow, 0), near = atomic_exchange(&ioNear, 0), small = atomic_exchange(&ioSmall, 0);
            if(reads) fprintf(stderr, "[WoWPS5 Wine] reads in 10 s: %llu (%.1f MiB) taking %.2f s in all; %llu over 0.2 ms; %llu right after the one before; %llu under 16 KiB\n",
                reads, (double)amount / 1048576, (double)spent / 1e9, slow, near, small);
        }
        const unsigned open = files + pipes + sockets + other;
        if(open > said + 64 || open + 64 < said) {
            said = open;
            // the five files with the most descriptors, as count x size: the size names the file
            char most_on[160]; int used = 0; most_on[0] = 0;
            for(unsigned place = 0; place < 5 && place < kinds; place++) {
                unsigned top = place;
                for(unsigned at = place + 1; at < kinds; at++) if(seen[at].count > seen[top].count) top = at;
                __typeof__(seen[0]) swap = seen[place]; seen[place] = seen[top]; seen[top] = swap;
                used += snprintf(most_on + used, sizeof(most_on) - (size_t)used, " %ux%lld", seen[place].count, (long long)seen[place].bytes);
            }
            fprintf(stderr, "[WoWPS5 Wine] descriptors: %u open of %u (files %u on %u different, of which memory objects %u on %u: %u files count against the 250; most:%s; pipes %u, sockets %u, other %u)\n",
                open, most, files, kinds, memory, memoryKinds, kinds - memoryKinds, most_on, pipes, sockets, other);
            if(getenv("WOWPS5_DESCRIPTOR_LIST")) {
                // how many more files the console would give now (a test run only: for a moment there are none left)
                fprintf(stderr, "[WoWPS5 Wine] descriptors: %d more files could be opened now\n", descriptorsLeft());       // every open file as descriptors x size, to name them by size on the PC
                char line[900]; int length = 0;
                for(unsigned at = 0; at < kinds; at++) {
                    length += snprintf(line + length, sizeof(line) - (size_t)length, " %ux%lld", seen[at].count, (long long)seen[at].bytes);
                    if(length > 840 || at + 1 == kinds) { fprintf(stderr, "[WoWPS5 Wine] descriptor list:%s\n", line); length = 0; }
                }
            }
        }
        sleep(10);
    }
    return NULL;
}
// WOWPS5_DESCRIPTOR_PROBE=1: what the console refuses, before Wine starts and
// with nothing else open. The game was refused files at about 1,150 descriptors
// of a limit said to be 13,952.
static void descriptorProbe(void) {
    enum { MOST = 8192 };
    static int kept[MOST], more[MOST];
    static const char* const file = "/data/wowps5/prefix/system.reg";
    int count = 0, extra = 0, error, extraError;
    errno = 0; while(count < MOST && (kept[count] = open(file, O_RDONLY)) >= 0) count++;
    error = count < MOST ? errno : 0;
    fprintf(stderr, "[WoWPS5 Wine] descriptor probe: one file opened %d times, then errno %d (highest %d)\n", count, error, count ? kept[count - 1] : -1);
    for(int i = 0; i < count; i++) close(kept[i]);
    count = 0; errno = 0; while(count + 1 < MOST && pipe(&kept[count]) == 0) count += 2;
    error = count + 1 < MOST ? errno : 0;
    errno = 0; while(extra < MOST && (more[extra] = open(file, O_RDONLY)) >= 0) extra++;
    extraError = extra < MOST ? errno : 0;
    fprintf(stderr, "[WoWPS5 Wine] descriptor probe: %d pipe ends, then errno %d; on top of them %d files, then errno %d\n", count, error, extra, extraError);
    for(int i = 0; i < extra; i++) close(more[i]);
    for(int i = 0; i < count; i++) close(kept[i]);
    // as the game has them: 620 pipe ends, then files
    count = 0; while(count < 620 && pipe(&kept[count]) == 0) count += 2;
    extra = 0; errno = 0; while(extra < MOST && (more[extra] = open(file, O_RDONLY)) >= 0) extra++;
    extraError = extra < MOST ? errno : 0;
    fprintf(stderr, "[WoWPS5 Wine] descriptor probe: with %d pipe ends held, %d files, then errno %d\n", count, extra, extraError);
    for(int i = 0; i < extra; i++) close(more[i]);
    // copies of one open file, as the server hands a file to its client
    const int one = open(file, O_RDONLY);
    extra = 0; errno = 0; while(extra < MOST && (more[extra] = wowps5_dup(one)) >= 0) extra++;
    extraError = extra < MOST ? errno : 0;
    fprintf(stderr, "[WoWPS5 Wine] descriptor probe: with %d pipe ends held, %d copies of one open file, then errno %d\n", count, extra, extraError);
    for(int i = 0; i < extra; i++) close(more[i]);
    close(one);
    for(int i = 0; i < count; i++) close(kept[i]);
    // Is it one budget, or one for each place? /data filled, then other places on top.
    count = 0; errno = 0; while(count < MOST && (kept[count] = open(file, O_RDONLY)) >= 0) count++;
    static const struct { const char* what; const char* path; int flags; } others[] = {
        { "the title's folder", "/app0/sce_sys/param.json", O_RDONLY },
        { "the title's own data folder", "/user/app/PPSA99220/sce_sys/param.json", O_RDONLY },
        { "a device", "/dev/null", O_RDWR },
        { "a directory on /data", "/data/wowps5", O_RDONLY | O_DIRECTORY },
        { "a system file", "/system_ex/app/PPSA99220/sce_sys/param.json", O_RDONLY },
        { "a new file on /data", "/data/wowps5/descriptor-probe.tmp", O_RDWR | O_CREAT },
    };
    for(unsigned kind = 0; kind < sizeof(others) / sizeof(*others); kind++) {
        extra = 0; errno = 0; while(extra < 1024 && (more[extra] = open(others[kind].path, others[kind].flags, 0600)) >= 0) extra++;
        extraError = extra < 1024 ? errno : 0;
        fprintf(stderr, "[WoWPS5 Wine] descriptor probe: /data full (%d); %s: %d more, then errno %d\n", count, others[kind].what, extra, extraError);
        for(int i = 0; i < extra; i++) close(more[i]);
    }
    unlink("/data/wowps5/descriptor-probe.tmp");
    for(int i = 0; i < count; i++) close(kept[i]);
    // Does a mapping keep its place once the descriptor is closed? Does an anonymous memory object take one?
    count = 0; errno = 0;
    for(; count < 1024; count++) {
        const int fd = open(file, O_RDONLY);
        if(fd < 0) break;
        void* view = mmap(NULL, 16384, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if(view == MAP_FAILED) { count = -count - 1; break; }
        kept[count] = 0;
    }
    fprintf(stderr, "[WoWPS5 Wine] descriptor probe: open, map, close: %d times, errno %d\n", count, errno);
    count = 0; errno = 0; while(count < 2048 && (kept[count] = shm_open(SHM_ANON, O_RDWR, 0600)) >= 0) count++;
    error = count < 2048 ? errno : 0;
    extra = 0; errno = 0; while(extra < MOST && (more[extra] = open(file, O_RDONLY)) >= 0) extra++;
    fprintf(stderr, "[WoWPS5 Wine] descriptor probe: %d anonymous memory objects, then errno %d; on top of them %d files\n", count, error, extra);
    for(int i = 0; i < extra; i++) close(more[i]);
    for(int i = 0; i < count; i++) close(kept[i]);
    // An anonymous memory object in place of a section's file: can it be told apart, sized, read and written?
    {
        const int a = shm_open(SHM_ANON, O_RDWR, 0600), b = shm_open(SHM_ANON, O_RDWR, 0600);
        struct stat sa, sb, sc; memset(&sa, 0, sizeof(sa)); memset(&sb, 0, sizeof(sb)); memset(&sc, 0, sizeof(sc));
        errno = 0; const int sized = ftruncate(a, 3 * 16384); const int sizedError = errno;
        const int sta = fstat(a, &sa), stb = fstat(b, &sb);
        const int copy = wowps5_dup(a); fstat(copy, &sc);
        char back[8] = {0};
        errno = 0; const ssize_t wrote = pwrite(a, "section", 8, 20000); const int wroteError = errno;
        errno = 0; const ssize_t got = pread(copy, back, 8, 20000); const int gotError = errno;
        errno = 0; const int grown = ftruncate(a, 40 * 16384); const int grownError = errno; struct stat sg; memset(&sg, 0, sizeof(sg)); fstat(a, &sg);
        fprintf(stderr, "[WoWPS5 Wine] descriptor probe: memory objects: fstat %d %d; a dev %llx ino %llx mode %o size %lld; b dev %llx ino %llx; copy of a ino %llx size %lld; "
            "ftruncate %d errno %d; pwrite %zd errno %d; pread %zd errno %d '%s'; grown %d errno %d size %lld\n",
            sta, stb, (unsigned long long)sa.st_dev, (unsigned long long)sa.st_ino, (unsigned)sa.st_mode, (long long)sa.st_size,
            (unsigned long long)sb.st_dev, (unsigned long long)sb.st_ino, (unsigned long long)sc.st_ino, (long long)sc.st_size,
            sized, sizedError, wrote, wroteError, got, gotError, back, grown, grownError, (long long)sg.st_size);
        close(a); close(b); close(copy);
    }
    // sockets: the server's side of every thread could be one pair instead of three pipes
    count = 0; errno = 0; while(count + 1 < MOST && socketpair(AF_UNIX, SOCK_STREAM, 0, &kept[count]) == 0) count += 2;
    error = count + 1 < MOST ? errno : 0;
    extra = 0; errno = 0; while(extra < MOST && (more[extra] = open(file, O_RDONLY)) >= 0) extra++;
    extraError = extra < MOST ? errno : 0;
    fprintf(stderr, "[WoWPS5 Wine] descriptor probe: %d socket-pair ends, then errno %d; on top of them %d files, then errno %d\n", count, error, extra, extraError);
    for(int i = 0; i < extra; i++) close(more[i]);
    for(int i = 0; i < count; i++) close(kept[i]);
}
// An earlier build could put an info label into the game as an add-on of its
// own interface. The label is gone; what it left on a console is taken away.
#define LABEL_FOLDER "/data/wowps5/prefix/dosdevices/c:/Games/World of Warcraft/_classic_beta_/Interface/AddOns/WoWPS5Info"
static void infoLabel(void) {
    remove(LABEL_FOLDER "/WoWPS5Info.toc"); remove(LABEL_FOLDER "/Info.lua"); remove(LABEL_FOLDER "/WoWPS5Info.lua");
    rmdir(LABEL_FOLDER);
}

// WOWPS5_CPU_PROBE=1: how many processors the title really gets, and how the
// system shares them when more threads want to run than there are processors.
static void* cpuProbeWork(void* argument) {
    uint64_t state = (uint64_t)(uintptr_t)argument * 0x9e3779b97f4a7c15ull + 1, sum = 0;
    for(unsigned i = 0; i < 150000000u; i++) { state ^= state << 13; state ^= state >> 7; state ^= state << 17; sum += state; }
    return (void*)(uintptr_t)(sum | 1);
}
static double cpuProbeNow(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (double)t.tv_sec + (double)t.tv_nsec / 1e9; }
static void cpuProbe(void) {
    int policy = -1; struct sched_param param; memset(&param, 0, sizeof(param));
    const int got = pthread_getschedparam(pthread_self(), &policy, &param);
    fprintf(stderr, "[WoWPS5 Wine] cpu probe: sysconf online %ld; this thread: getschedparam %d policy %d priority %d (FIFO=%d RR=%d OTHER=%d); "
        "priority ranges FIFO %d..%d RR %d..%d OTHER %d..%d\n", sysconf(_SC_NPROCESSORS_ONLN), got, policy, param.sched_priority,
        SCHED_FIFO, SCHED_RR, SCHED_OTHER, sched_get_priority_min(SCHED_FIFO), sched_get_priority_max(SCHED_FIFO),
        sched_get_priority_min(SCHED_RR), sched_get_priority_max(SCHED_RR), sched_get_priority_min(SCHED_OTHER), sched_get_priority_max(SCHED_OTHER));
    static const struct { const char* what; int policy; int count; } runs[] = {
        {"as created", -1, 1}, {"as created", -1, 12}, {"as created", -1, 20}, {"round robin", SCHED_RR, 20}, {"time sharing", SCHED_OTHER, 20},
    };
    double one = 0;
    for(unsigned r = 0; r < sizeof(runs) / sizeof(*runs); r++) {
        pthread_t threads[20]; int made = 0, refused = 0;
        const double begin = cpuProbeNow();
        for(int i = 0; i < runs[r].count; i++) {
            pthread_attr_t attr; pthread_attr_init(&attr);
            if(runs[r].policy >= 0) {
                struct sched_param wanted = param;
                if(runs[r].policy == SCHED_OTHER) wanted.sched_priority = sched_get_priority_min(SCHED_OTHER);
                pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
                pthread_attr_setschedpolicy(&attr, runs[r].policy);
                pthread_attr_setschedparam(&attr, &wanted);
            }
            int error = pthread_create(&threads[made], &attr, cpuProbeWork, (void*)(uintptr_t)(i + 1));
            if(error && runs[r].policy >= 0) { refused = error; pthread_attr_destroy(&attr); break; }
            if(!error) made++;
            pthread_attr_destroy(&attr);
        }
        // while they work: how late does a thread that sleeps two milliseconds wake?
        double worst = 0; int sleeps = 0;
        for(; sleeps < 40 && made; sleeps++) {
            const double before = cpuProbeNow(); usleep(2000); const double late = cpuProbeNow() - before - 0.002;
            if(late > worst) worst = late;
            if(cpuProbeNow() - begin > 0.2) break;
        }
        for(int i = 0; i < made; i++) pthread_join(threads[i], NULL);
        const double seconds = cpuProbeNow() - begin;
        if(r == 0) one = seconds;
        fprintf(stderr, "[WoWPS5 Wine] cpu probe: %2d threads %s: %.3f s (%.1f times one thread's work); a 2 ms sleep woke up to %.1f ms late%s\n",
            made, runs[r].what, seconds, made ? one * made / seconds : 0, worst * 1000, refused ? " -- the policy was refused" : "");
        if(refused) fprintf(stderr, "[WoWPS5 Wine] cpu probe: pthread_create with %s: error %d\n", runs[r].what, refused);
    }
}

// WOWPS5_IO_PROBE=<file>: how fast the console reads a large file of the game:
// in order, and in small pieces from places all over it.
static void ioProbe(const char* path) {
    const int fd = open(path, O_RDONLY);
    struct stat info;
    if(fd < 0 || fstat(fd, &info)) { fprintf(stderr, "[WoWPS5 Wine] io probe: cannot open %s\n", path); return; }
    static char block[1 << 20];
    const off_t size = info.st_size;
    uint64_t state = (uint64_t)size ^ 0x9e3779b97f4a7c15ull;
    for(int pass = 0; pass < 2; pass++) {
        const off_t start = pass ? 0 : (size / 2) & ~(off_t)0xfffff;      // two different places: the second pass is not the first one's cache
        double begin = cpuProbeNow(); off_t done = 0;
        while(done < (off_t)256 << 20 && start + done < size) { const ssize_t got = pread(fd, block, sizeof(block), start + done); if(got <= 0) break; done += got; }
        double seconds = cpuProbeNow() - begin;
        fprintf(stderr, "[WoWPS5 Wine] io probe: in order, 1 MiB at a time from %lld MiB: %.0f MiB in %.3f s = %.0f MiB/s\n",
            (long long)(start >> 20), (double)done / 1048576, seconds, (double)done / 1048576 / seconds);
        static const size_t pieces[] = {4096, 65536};
        for(unsigned k = 0; k < 2; k++) {
            begin = cpuProbeNow(); double worst = 0;
            for(int i = 0; i < 2000; i++) {
                state ^= state << 13; state ^= state >> 7; state ^= state << 17;
                const off_t at = (off_t)(state % (uint64_t)(size - (off_t)pieces[k])) & ~(off_t)0xfff;
                const double before = cpuProbeNow();
                if(pread(fd, block, pieces[k], at) <= 0) break;
                const double took = cpuProbeNow() - before; if(took > worst) worst = took;
            }
            seconds = cpuProbeNow() - begin;
            fprintf(stderr, "[WoWPS5 Wine] io probe: 2000 reads of %zu KiB from random places: %.0f microseconds each, the slowest %.1f ms\n",
                pieces[k] / 1024, seconds / 2000 * 1e6, worst * 1000);
        }
    }
    close(fd);
}

// The console starts a title's threads first-in-first-out at one priority: a
// thread runs until it blocks, and with more busy threads than the 13
// processors a title has, the others wait for one to finish (measured: a thread
// that slept 2 ms woke 306 ms late beside 20 busy ones; 2.9 ms late when they
// were round robin, and the 20 did a quarter more work). Windows programs are
// written for time slices, and this one runs more threads than that while it
// loads. With WOWPS5_SCHED=rr the thread that starts Wine is made round robin
// here, and every thread made after it inherits that.
//
// OFF unless asked for: the one session played with it froze the console when
// the game was closed from the system menu. The shell's sceApplicationSuspend
// of the title timed out after 60 seconds, the Shell UI's heartbeat stopped,
// and the console had to be restarted (2026-10-05). Closing had worked in every
// session before it. Why round-robin threads cannot be suspended is not known.
static void shareProcessors(void) {
    const char* wanted = getenv("WOWPS5_SCHED");
    int policy = -1; struct sched_param param; memset(&param, 0, sizeof(param));
    if(!wanted || strcmp(wanted, "rr") || pthread_getschedparam(pthread_self(), &policy, &param)) return;
    const int result = pthread_setschedparam(pthread_self(), SCHED_RR, &param);
    fprintf(stderr, "[WoWPS5 Wine] scheduling: round robin at priority %d (the console's policy was %d): %s\n",
        param.sched_priority, policy, result ? strerror(result) : "set");
}

static void* splashThread(void* unused) {
    (void)unused;
    for(unsigned left = 30; left; ) left = sleep(left);
    wowps5_first_frame();
    return NULL;
}

void wowps5WineStartIfRequested(const char* requestPath) {
    FILE* request = fopen(requestPath, "r");
    const bool requested = request != NULL;
    if(!request && (access(PROBE_RUN, R_OK) == 0 || access(GAME_PROGRAM, R_OK) != 0)) return;
    static char program[1024]; char line[1024];
    bool have = true;
    if(requested) {
        have = fgets(program, sizeof(program), request) != NULL;
        program[strcspn(program, "\r\n")] = 0;
    } else {
        snprintf(program, sizeof(program), "%s", GAME_PROGRAM);
        setenv("WINEDLLOVERRIDES", "d3d12,d3d12core,dxgi=n;winemenubuilder.exe=d", 1);
        setenv("VKD3D_CONFIG", "single_queue", 1);
        setenv("WINEDEBUG", "err+all,fixme-all", 1);
        request = fopen(LAUNCH_SETTINGS, "r");
    }
    // Defaults, each replaceable by a NAME=value line of the request
    setenv("WINEPS5ROOT", "/data/wowps5/wine", 1);
    setenv("WINEPREFIX", "/data/wowps5/prefix", 1);
    setenv("HOME", "/data/wowps5", 1);
    setenv("USER", "wowps5", 1);
    setenv("WINEBOOTSTRAPMODE", "1", 1);    // builtin DLLs load from the installation: there is no wineboot here
    setenv("WINELOADERNOEXEC", "1", 1);
    static char* arguments[32]; int count = 2;
    while(request && fgets(line, sizeof(line), request)) {
        line[strcspn(line, "\r\n")] = 0;
        char* equals = strchr(line, '=');
        if(line[0] == '#') continue;
        if(line[0] == '+') { if(count < 31) arguments[count++] = strdup(line + 1); }   // an argument for the program
        else if(equals) { *equals = 0; setenv(line, equals + 1, 1); }
    }
    if(request) fclose(request);
    if(requested) remove(requestPath);       // the request is for this launch only
    if(!have || !program[0]) { fprintf(stderr, "[WoWPS5 Wine] empty run request\n"); return; }
    // A requested run is a test and always has a time limit. The game started
    // from the home screen has one only if the launch settings give it.
    if(!requested) infoLabel();
    heldOff = getenv("WOWPS5_WRITE_BEHIND") && !strcmp(getenv("WOWPS5_WRITE_BEHIND"), "0");
    const char* deadline = getenv("WOWPS5_DEADLINE");
    const unsigned limit = deadline && atoi(deadline) > 0 ? (unsigned)atoi(deadline) : requested ? 600 : 0;
    pthread_t watcher;
    if(limit && !pthread_create(&watcher, NULL, deadlineThread, (void*)(uintptr_t)limit)) pthread_detach(watcher);
    if(!pthread_create(&watcher, NULL, splashThread, NULL)) pthread_detach(watcher);
    shareProcessors();
    ioStats = getenv("WOWPS5_IO_STATS") != NULL;
    if(getenv("WOWPS5_DESCRIPTOR_PROBE")) descriptorProbe();
    if(getenv("WOWPS5_CPU_PROBE")) cpuProbe();
    if(getenv("WOWPS5_IO_PROBE")) ioProbe(getenv("WOWPS5_IO_PROBE"));
    if(!pthread_create(&watcher, NULL, descriptorThread, NULL)) pthread_detach(watcher);
    fprintf(stderr, "[WoWPS5 Wine] starting %s (root %s, prefix %s, at most %u s)\n", program, getenv("WINEPS5ROOT"), getenv("WINEPREFIX"), limit);
    fflush(stderr);
    wowps5_input_start();
    static char name[] = "wine";
    arguments[0] = name; arguments[1] = program; arguments[count] = NULL;
    __wine_main(count, arguments);
}
#endif
