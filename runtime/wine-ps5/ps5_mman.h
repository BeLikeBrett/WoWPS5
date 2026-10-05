/* BSD mmap semantics for Wine on the PS5's memory model. MIT license.
 *
 * What the console measured (docs/technical-notes.md, evidence/wine-contract-v1):
 * plain mmap draws on a 417 MiB flexible budget, cannot reserve 1 GiB, lands in
 * the GPU window without an address, and never yields executable memory.
 * Reservations (sceKernelReserveVirtualRange) and direct memory mapped at fixed
 * addresses do everything Wine's virtual.c asks of mmap, in 16 KiB pages and
 * 64 KiB direct-memory units.
 *
 * This layer gives Wine's Unix side the calls it is written against:
 *   - anonymous PROT_NONE mappings are reservations;
 *   - anonymous memory with access is direct memory, zeroed, mapped page by
 *     page from the 64 KiB unit that covers it; mprotect of a reserved page
 *     commits it, as an overcommitting kernel would;
 *   - PROT_EXEC is real: it is granted by a protection change after the map;
 *   - file mappings stay the kernel's own, placed by this layer, until one
 *     needs execute, which file pages never have on the console:
 *       - a shared mapping (the two views of one section that a program
 *         writes code through and runs it from) becomes direct memory mapped
 *         at each of its views, loaded from the file and written back to it
 *         when the last view goes;
 *       - a private mapping is refused with ENODEV when mapped executable, so
 *         Wine reads the file into anonymous memory instead (its path for
 *         filesystems without mmap), and one made executable later becomes
 *         an anonymous copy of what it held.
 *
 * Every page the layer has handed out is in its own table, so a call on a
 * mixed range does the right thing for each page, and a unit's direct memory
 * goes back when its last committed page does.
 */
#ifndef WOWPS5_PS5_MMAN_H
#define WOWPS5_PS5_MMAN_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS5W_PAGE ((size_t)0x4000)
#define PS5W_UNIT ((size_t)0x10000)
/* Nothing at or above 1 TiB is granted by the console. */
#define PS5W_ADDRESS_LIMIT ((uintptr_t)1 << 40)

/* What the layer needs from the kernel. The console's are sceKernel* calls;
 * the host test's are a model on Linux mmap. Each returns 0 or an error. */
struct ps5w_kernel {
   /* fixed: exactly at *address, replacing what is there. Otherwise *address
    * is a hint (or NULL) and receives the placement. */
   int (*reserve)(void **address, size_t bytes, int fixed);
   int (*allocate)(size_t bytes, int64_t *start);
   int (*release)(int64_t start, size_t bytes);
   /* Direct memory at a fixed address, read-write, replacing what is there. */
   int (*map)(void *address, size_t bytes, int64_t start);
   int (*protect)(void *address, size_t bytes, int prot);
   int (*unmap)(void *address, size_t bytes);
   /* The same direct memory at one more address, read-write: every address
    * mapped this way from one allocation shows the same bytes. */
   int (*alias)(void *address, size_t bytes, int64_t start);
};

/* Once, before any other call. NULL selects the console's kernel, or on the
 * host its model (ps5_mman_model.c). */
void ps5w_init(const struct ps5w_kernel *kernel);
#ifndef __PROSPERO__
const struct ps5w_kernel *ps5w_host_model(void);
#endif

void *ps5w_mmap(void *address, size_t bytes, int prot, int flags, int fd, off_t offset);
int ps5w_munmap(void *address, size_t bytes);
int ps5w_mprotect(void *address, size_t bytes, int prot);
int ps5w_madvise(void *address, size_t bytes, int advice);
int ps5w_msync(void *address, size_t bytes, int flags);
int ps5w_mlock(const void *address, size_t bytes);
int ps5w_munlock(const void *address, size_t bytes);

enum ps5w_kind { PS5W_FREE = 0, PS5W_RESERVED = 1, PS5W_COMMITTED = 2, PS5W_FILE = 3 };
/* The layer's record of one page: its kind, and its PROT_* bits in *prot. */
/* A Windows section with no file has to have a descriptor all the same, for
 * the server to hand out and for views to be made from. On the console that
 * descriptor is a file made for it, and its views, mapped by the kernel, were
 * pages for the kernel to write to storage: 240 MiB of them kept a game from
 * exiting for more than a minute, at which the console ended one of its own
 * services and froze. So the server says which files are such sections
 * (ps5w_section, after giving the file its size), and their contents live in
 * direct memory from the first view on, shared by every view; the file is
 * never read or written. The memory stays while the section is open or has a
 * view (ps5w_section_closed says it has been closed). */
int ps5w_section(int fd);
void ps5w_section_closed(int fd);

/* Writes what views of files have changed to the files, before the process
 * asks to be closed (the console gives a closing process a minute). */
void ps5w_flush(void);

enum ps5w_kind ps5w_query(const void *address, int *prot);

struct ps5w_stats {
   uint64_t reserved_pages, committed_pages, file_pages;
   uint64_t direct_bytes; /* direct memory held now */
   uint64_t allocations;  /* kernel direct-memory allocations made, ever */
   uint64_t shared_objects; /* files held in direct memory for their executable views, now */
};
void ps5w_live(struct ps5w_stats *stats);

#ifdef __cplusplus
}
#endif

#endif
