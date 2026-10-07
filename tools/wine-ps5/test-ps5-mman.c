/* Host test of runtime/wine-ps5/ps5_mman.c against a model of the console's kernel. MIT license.
 *
 * The model (runtime/wine-ps5/ps5_mman_model.c) counts a release it did not
 * allocate, so a double or missing release fails the test. This establishes
 * the layer's bookkeeping, nothing about the console.
 *
 *   tools/wine-ps5/test-ps5-mman.sh
 */
#define _GNU_SOURCE
#include "ps5_mman.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern int64_t ps5w_model_held;
extern unsigned ps5w_model_faults;
static unsigned checks, failed;

#define CHECK(condition, ...)                                 \
   do {                                                       \
      checks++;                                               \
      if (!(condition)) {                                     \
         failed++;                                            \
         printf("FAIL %s:%d: ", __FILE__, __LINE__);          \
         printf(__VA_ARGS__);                                 \
         printf("\n");                                        \
      }                                                       \
   } while (0)

static void
balance(const char *when)
{
   struct ps5w_stats stats;
   ps5w_live(&stats);
   CHECK((int64_t)stats.direct_bytes == ps5w_model_held, "%s: layer holds %llu, model %lld", when,
         (unsigned long long)stats.direct_bytes, (long long)ps5w_model_held);
}

/* The calls Wine's virtual.c makes, in the order it makes them. */
static void
scripted(void)
{
   uint8_t *const base = (uint8_t *)0x1000000000ull;
   const size_t area = 16u << 20;
   struct ps5w_stats stats;
   int prot = -1;

   /* reserve_area: anon_mmap_tryfixed(PROT_NONE) */
   CHECK(ps5w_mmap(base, area, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == base, "reserve");
   CHECK(ps5w_query(base + area - 1, &prot) == PS5W_RESERVED && prot == 0, "reserved to its last page");
   CHECK(ps5w_query(base + area, NULL) == PS5W_FREE, "nothing past the area");
   errno = 0;
   CHECK(ps5w_mmap(base + 0x10000, 0x10000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED &&
            errno == EEXIST, "a second exclusive mapping is refused");

   /* NtAllocateVirtualMemory(MEM_COMMIT) of one page inside a reserved view: mprotect */
   CHECK(ps5w_mprotect(base + 0x4000, 0x1000, PROT_READ | PROT_WRITE) == 0, "commit by mprotect");
   CHECK(ps5w_query(base + 0x4000, &prot) == PS5W_COMMITTED && prot == (PROT_READ | PROT_WRITE), "the page is committed");
   CHECK(ps5w_query(base, NULL) == PS5W_RESERVED && ps5w_query(base + 0x8000, NULL) == PS5W_RESERVED, "its neighbours stay reserved");
   bool zero = true;
   for (size_t at = 0; at < PS5W_PAGE; at++)
      zero &= base[0x4000 + at] == 0;
   CHECK(zero, "committed memory is zeroed although direct memory came back dirty");
   ps5w_live(&stats);
   CHECK(stats.direct_bytes == PS5W_UNIT && stats.committed_pages == 1, "one unit backs one page");
   memset(base + 0x4000, 0x77, PS5W_PAGE);

   /* a second page of the same unit shares its direct memory */
   CHECK(ps5w_mprotect(base + 0xc000, 0x4000, PROT_READ | PROT_WRITE) == 0, "second page");
   ps5w_live(&stats);
   CHECK(stats.direct_bytes == PS5W_UNIT && stats.allocations == 1, "the unit is not allocated twice");
   base[0xc000] = 0x33;

   /* protection changes keep contents */
   CHECK(ps5w_mprotect(base + 0x4000, 0x4000, PROT_READ) == 0 && base[0x4000] == 0x77, "read-only keeps the page");
   CHECK(ps5w_mprotect(base + 0x4000, 0x4000, PROT_NONE) == 0 && ps5w_query(base + 0x4000, &prot) == PS5W_COMMITTED && prot == 0,
         "no access is still committed");
   CHECK(ps5w_mprotect(base + 0x4000, 0x4000, PROT_READ | PROT_WRITE) == 0 && base[0x4000] == 0x77 && base[0x7fff] == 0x77,
         "access returns with the contents");

   /* MEM_DECOMMIT: anon_mmap_fixed(PROT_NONE) over the page */
   CHECK(ps5w_mmap(base + 0x4000, 0x4000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == base + 0x4000, "decommit");
   ps5w_live(&stats);
   CHECK(stats.direct_bytes == PS5W_UNIT && base[0xc000] == 0x33, "the unit stays while its other page is committed");
   CHECK(ps5w_mmap(base + 0xc000, 0x4000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == base + 0xc000, "decommit the other");
   ps5w_live(&stats);
   CHECK(stats.direct_bytes == 0 && stats.committed_pages == 0, "the unit's memory goes back with its last page");
   CHECK(ps5w_mprotect(base + 0x4000, 0x4000, PROT_READ | PROT_WRITE) == 0 && base[0x4000] == 0, "a recommitted page is zero again");
   balance("after recommit");

   /* a view of whole units: one allocation, however large */
   const uint64_t before = (ps5w_live(&stats), stats.allocations);
   CHECK(ps5w_mmap(base + 0x100000, 0x400000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == base + 0x100000, "4 MiB view");
   ps5w_live(&stats);
   CHECK(stats.allocations == before + 1, "64 whole units came from one allocation, not %llu", (unsigned long long)(stats.allocations - before));
   memset(base + 0x100000, 0x11, 0x400000);
   /* fresh memory over committed pages reads zero */
   CHECK(ps5w_mmap(base + 0x100000, 0x20000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == base + 0x100000 &&
            base[0x100000] == 0 && base[0x11ffff] == 0 && base[0x120000] == 0x11, "a new mapping over a committed range is zero");
   /* freeing the middle of that view releases just those units */
   CHECK(ps5w_munmap(base + 0x200000, 0x100000) == 0 && ps5w_query(base + 0x200000, NULL) == PS5W_FREE, "unmap the middle");
   CHECK(base[0x1fffff] == 0x11 && base[0x300000] == 0x11, "both sides keep their contents");
   balance("after a partial release of one allocation");
   errno = 0;
   CHECK(ps5w_mprotect(base + 0x200000, 0x4000, PROT_READ) == -1 && errno == ENOMEM, "mprotect of unmapped memory is refused");

   /* MEM_RESET */
   CHECK(ps5w_madvise(base + 0x300000, 0x8000, MADV_DONTNEED) == 0 && base[0x300000] == 0 && base[0x308000] == 0x11, "MADV_DONTNEED zeroes");

   /* anon_mmap_alloc: no address */
   uint8_t *const anywhere = ps5w_mmap(NULL, 0x5000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   CHECK(anywhere != MAP_FAILED && (uintptr_t)anywhere % PS5W_UNIT == 0 && anywhere[0] == 0 && anywhere[0x7fff] == 0, "a placed mapping, whole pages");
   CHECK(ps5w_munmap(anywhere, 0x5000) == 0 && ps5w_query(anywhere + 0x4000, NULL) == PS5W_FREE, "and its release");

   /* a file view inside the reserved area, then anonymous memory over it */
   char path[] = "/tmp/ps5w-test-XXXXXX";
   const int file = mkstemp(path);
   char text[0x8000];
   memset(text, 'f', sizeof(text));
   CHECK(file >= 0 && write(file, text, sizeof(text)) == (ssize_t)sizeof(text), "the test file");
   errno = 0;
   CHECK(ps5w_mmap(base + 0x800000, 0x8000, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, file, 0) == MAP_FAILED && errno == ENODEV,
         "an executable file mapping is refused with ENODEV, Wine's cue to read instead");
   CHECK(ps5w_query(base + 0x800000, NULL) == PS5W_RESERVED, "and leaves the range as it was");
   CHECK(ps5w_mmap(base + 0x800000, 0x8000, PROT_READ, MAP_PRIVATE | MAP_FIXED, file, 0) == base + 0x800000 && base[0x800000] == 'f' &&
            base[0x807fff] == 'f', "a file view");
   CHECK(ps5w_query(base + 0x804000, &prot) == PS5W_FILE && prot == PROT_READ, "recorded as the kernel's own");
   CHECK(ps5w_mprotect(base + 0x800000, 0x8000, PROT_READ | PROT_WRITE) == 0 && (base[0x800000] = 'g') == 'g', "copy-on-write through mprotect");
   CHECK(ps5w_mprotect(base + 0x804000, 0x8000, PROT_READ | PROT_WRITE) == 0 && ps5w_query(base + 0x808000, NULL) == PS5W_COMMITTED,
         "one mprotect across a file page and a reserved page handles each");
   CHECK(ps5w_mmap(base + 0x800000, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == base + 0x800000 &&
            base[0x800000] == 0 && base[0x804000] == 'f', "anonymous memory replaces one file page only");
   uint8_t *const placed = ps5w_mmap(NULL, 0x8000, PROT_READ, MAP_SHARED, file, 0);
   CHECK(placed != MAP_FAILED && placed[0] == 'f' && ps5w_query(placed, NULL) == PS5W_FILE, "a file view with no address is placed");
   CHECK(ps5w_munmap(placed, 0x8000) == 0, "and released");
   close(file);
   unlink(path);

   /* unmap_area over everything: reserved, committed, file and free pages at once */
   CHECK(ps5w_munmap(base, area) == 0, "unmap the whole area");
   ps5w_live(&stats);
   CHECK(!stats.direct_bytes && !stats.committed_pages && !stats.reserved_pages && !stats.file_pages, "nothing is left");
   balance("at the end of the script");
   CHECK(ps5w_mmap(base, area, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == base, "the range can be taken again");
   CHECK(ps5w_munmap(base, area) == 0, "and given back");
}

/* Random calls on a small window against a per-page reference. */
#define WINDOW_PAGES 64
static void
randomised(unsigned seed, unsigned steps)
{
   uint8_t *const base = (uint8_t *)0x2000000000ull;
   int kind[WINDOW_PAGES] = {0}, prot[WINDOW_PAGES] = {0};
   uint64_t token[WINDOW_PAGES] = {0};
   srand(seed);
   for (unsigned step = 0; step < steps; step++) {
      const unsigned first = (unsigned)rand() % WINDOW_PAGES, count = 1 + (unsigned)rand() % (WINDOW_PAGES - first > 12 ? 12 : WINDOW_PAGES - first);
      uint8_t *const at = base + first * PS5W_PAGE;
      const size_t bytes = count * PS5W_PAGE;
      static const int prots[] = {PROT_NONE, PROT_READ, PROT_READ | PROT_WRITE};
      const int wanted = prots[rand() % 3];
      switch (rand() % 5) {
      case 0: /* reserve */
         CHECK(ps5w_mmap(at, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == at, "step %u reserve", step);
         for (unsigned p = first; p < first + count; p++)
            kind[p] = PS5W_RESERVED, prot[p] = 0, token[p] = 0;
         break;
      case 1: /* fresh memory */
         CHECK(ps5w_mmap(at, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == at, "step %u map", step);
         for (unsigned p = first; p < first + count; p++)
            kind[p] = PS5W_COMMITTED, prot[p] = PROT_READ | PROT_WRITE, token[p] = 0;
         break;
      case 2: { /* protect: applies page runs in order and stops at the first free page */
         bool refused = false;
         for (unsigned p = first; p < first + count && !refused; p++) {
            if (kind[p] == PS5W_FREE)
               refused = true;
            else if (kind[p] == PS5W_COMMITTED)
               prot[p] = wanted;
            else if (wanted)
               kind[p] = PS5W_COMMITTED, prot[p] = wanted, token[p] = 0;
         }
         CHECK((ps5w_mprotect(at, bytes, wanted) == 0) == !refused, "step %u protect", step);
         break;
      }
      case 3: /* unmap */
         CHECK(ps5w_munmap(at, bytes) == 0, "step %u unmap", step);
         for (unsigned p = first; p < first + count; p++)
            kind[p] = PS5W_FREE, prot[p] = 0, token[p] = 0;
         break;
      case 4: /* write */
         for (unsigned p = first; p < first + count; p++)
            if (kind[p] == PS5W_COMMITTED && (prot[p] & PROT_WRITE)) {
               token[p] = ((uint64_t)rand() << 32) | (uint64_t)rand() | 1;
               memcpy(base + p * PS5W_PAGE, &token[p], 8);
               memcpy(base + (p + 1) * PS5W_PAGE - 8, &token[p], 8);
            }
         break;
      }
      unsigned units = 0;
      for (unsigned p = 0; p < WINDOW_PAGES; p++) {
         int layer_prot = -1;
         const int layer_kind = ps5w_query(base + p * PS5W_PAGE, &layer_prot);
         CHECK(layer_kind == kind[p] && layer_prot == prot[p], "step %u page %u: kind %d prot %d, expected %d %d", step, p, layer_kind,
               layer_prot, kind[p], prot[p]);
         if (kind[p] == PS5W_COMMITTED && (prot[p] & PROT_READ)) {
            uint64_t head, tail;
            memcpy(&head, base + p * PS5W_PAGE, 8);
            memcpy(&tail, base + (p + 1) * PS5W_PAGE - 8, 8);
            CHECK(head == token[p] && tail == token[p], "step %u page %u: contents", step, p);
         }
         if (p % 4 == 0)
            units += kind[p] == PS5W_COMMITTED || kind[p + 1] == PS5W_COMMITTED || kind[p + 2] == PS5W_COMMITTED || kind[p + 3] == PS5W_COMMITTED;
      }
      struct ps5w_stats stats;
      ps5w_live(&stats);
      CHECK(stats.direct_bytes == units * PS5W_UNIT, "step %u: %llu bytes of direct memory for %u units in use", step,
            (unsigned long long)stats.direct_bytes, units);
      if (failed > 20)
         return;
   }
   CHECK(ps5w_munmap(base, WINDOW_PAGES * PS5W_PAGE) == 0, "release the window");
   balance("after the random run");
}

/* A section mapped twice, written through one view and run through the other:
 * what a program does that makes its own code and never holds memory that is
 * writable and executable at once. */
static void
shared_sections(void)
{
   typedef int (*function)(void);
   static const uint8_t code[] = {0xb8, 42, 0, 0, 0, 0xc3}; /* mov eax, 42; ret */
   struct ps5w_stats stats;
   char path[] = "/tmp/ps5-mman-shared-XXXXXX";
   const int file = mkstemp(path);
   unlink(path);
   CHECK(file >= 0 && ftruncate(file, 0x1e800) == 0 && pwrite(file, "head", 4, 0) == 4 && pwrite(file, "tail", 4, 0x1e7fc) == 4,
         "the section's file, which ends inside a page");

   uint8_t *const writable = ps5w_mmap(NULL, 0x20000, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
   ps5w_live(&stats);
   CHECK(writable != MAP_FAILED && !memcmp(writable, "head", 4) && !stats.shared_objects && !stats.direct_bytes,
         "a writable shared view alone is the kernel's file mapping");
   memcpy(writable + 0x4000, code, sizeof(code));

   uint8_t *const runnable = ps5w_mmap(NULL, 0x8000, PROT_READ | PROT_EXEC, MAP_SHARED, file, 0x4000);
   int prot = -1;
   ps5w_live(&stats);
   CHECK(runnable != MAP_FAILED && stats.shared_objects == 1 && stats.direct_bytes == 0x20000, "an executable shared view makes an object of the file");
   CHECK(runnable != MAP_FAILED && ps5w_query(runnable, &prot) == PS5W_FILE && prot == (PROT_READ | PROT_EXEC), "recorded as a file view that runs");
   CHECK(runnable != MAP_FAILED && !memcmp(runnable, code, sizeof(code)) && ((function)runnable)() == 42,
         "what was written through the first view runs through the second");
   CHECK(!memcmp(writable, "head", 4) && !memcmp(writable + 0x1e7fc, "tail", 4), "the first view kept the file's contents when it moved");
   writable[0x4001] = 43;
   CHECK(((function)runnable)() == 43, "and a later write is seen at once");
   balance("with an object");

   uint8_t *const reader = ps5w_mmap(NULL, 0x4000, PROT_READ, MAP_SHARED, file, 0x1c000);
   writable[0x1c010] = 9;
   CHECK(reader != MAP_FAILED && reader[0x10] == 9 && !memcmp(reader + 0x27fc, "tail", 4), "a later plain view of the file is of the object too");
   CHECK(reader != MAP_FAILED && reader[0x2800] == 0 && reader[0x3fff] == 0, "and the page is zero past the file's end");
   CHECK(ps5w_mmap(NULL, 0x4000, PROT_READ, MAP_SHARED, file, 0x20000) == MAP_FAILED, "a view past the object's end is refused");

   /* a hole in a view: both sides stay views of the object */
   CHECK(ps5w_munmap(writable + 0x8000, 0x4000) == 0 && ps5w_query(writable + 0x8000, NULL) == PS5W_FREE, "unmap a page inside a view");
   writable[0x4001] = 44;
   writable[0xc000] = 5;
   CHECK(((function)runnable)() == 44 && writable[0x1c010] == 9, "the parts on both sides still show the object");
   /* anonymous memory over a page of a view */
   CHECK(ps5w_mmap(writable + 0x10000, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == writable + 0x10000 &&
            ps5w_query(writable + 0x10000, NULL) == PS5W_COMMITTED && writable[0x10000] == 0, "anonymous memory replaces a page of a view");
   writable[0x10000] = 1;
   CHECK(ps5w_mprotect(runnable, 0x8000, PROT_READ) == 0 && ps5w_mprotect(runnable, 0x8000, PROT_READ | PROT_EXEC) == 0 &&
            ((function)runnable)() == 44, "protection changes on a view of an object");

   CHECK(ps5w_munmap(runnable, 0x8000) == 0 && ps5w_munmap(reader, 0x4000) == 0, "unmap two views");
   ps5w_live(&stats);
   CHECK(stats.shared_objects == 1, "the object stays while a view is left");
   CHECK(ps5w_munmap(writable, 0x8000) == 0, "unmap the part below the hole");
   ps5w_live(&stats);
   CHECK(stats.shared_objects == 1 && writable[0xc000] == 5, "the part above it is a view of its own and keeps the object");
   CHECK(ps5w_munmap(writable, 0x20000) == 0, "unmap the last, with the anonymous page in it");
   ps5w_live(&stats);
   CHECK(!stats.shared_objects && !stats.direct_bytes && !stats.file_pages && !stats.committed_pages, "the object goes with its last view");
   static uint8_t back[0x1e800 + 1];
   CHECK(pread(file, back, sizeof(back), 0) == 0x1e800 && back[0x4001] == 44 && back[0xc000] == 5 && back[0x1c010] == 9 &&
            !memcmp(back, "head", 4) && !memcmp(back + 0x1e7fc, "tail", 4) && back[0x10000] == 0,
         "and the file has what the views held");
   balance("after the object");

   /* the same through protection changes: views mapped plain, one made executable later */
   uint8_t *const first = ps5w_mmap(NULL, 0x20000, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
   uint8_t *const second = ps5w_mmap(NULL, 0x20000, PROT_READ, MAP_SHARED, file, 0);
   CHECK(first != MAP_FAILED && second != MAP_FAILED && second[0x4001] == 44, "two plain views");
   CHECK(ps5w_munmap(second, 0x4000) == 0 && ps5w_munmap(first, 0x4000) == 0 && ps5w_munmap(second + 0xc000, 0x4000) == 0,
         "each loses its first page, and one a page inside");
   CHECK(ps5w_mprotect(second + 0x4000, 0x4000, PROT_READ | PROT_EXEC) == 0, "one page of the read-only view made executable");
   ps5w_live(&stats);
   CHECK(stats.shared_objects == 1 && ((function)(second + 0x4000))() == 44, "it runs, from an object");
   first[0x4001] = 45;
   first[0x18000] = 7;
   CHECK(((function)(second + 0x4000))() == 45 && second[0x18000] == 7, "and both whole views are of the object");
   CHECK(first[0x1e7fc] == 't' && second[0x1e7fc] == 't', "at the right place in it");
   CHECK(ps5w_munmap(first, 0x20000) == 0 && ps5w_munmap(second, 0x20000) == 0, "unmap both");
   CHECK(pread(file, back, 2, 0x4000) == 2 && back[1] == 45, "written back again");

   /* a file nobody can write through: its executable view is a copy */
   const int reading = open("/proc/self/exe", O_RDONLY);
   uint8_t *const image = ps5w_mmap(NULL, 0x8000, PROT_READ, MAP_PRIVATE, reading, 0);
   uint8_t *const kept = ps5w_mmap(NULL, 0x8000, PROT_READ, MAP_SHARED, reading, 0);
   CHECK(image != MAP_FAILED && kept != MAP_FAILED && !memcmp(image, "\177ELF", 4), "private and shared read-only views");
   CHECK(ps5w_mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_SHARED, reading, 0) == MAP_FAILED && errno == EACCES,
         "no writable shared view of a file opened for reading");
   CHECK(ps5w_mprotect(image, 0x4000, PROT_READ | PROT_EXEC) == 0 && ps5w_query(image, &prot) == PS5W_COMMITTED &&
            prot == (PROT_READ | PROT_EXEC) && !memcmp(image, kept, 0x4000) && ps5w_query(image + 0x4000, NULL) == PS5W_FILE,
         "a private view made executable is a copy in direct memory");
   CHECK(ps5w_mprotect(kept, 0x8000, PROT_READ | PROT_EXEC) == 0 && ps5w_query(kept, NULL) == PS5W_COMMITTED && !memcmp(kept, image, 0x4000),
         "and so is a shared one with no writer");
   ps5w_live(&stats);
   CHECK(!stats.shared_objects, "no object for either");
   uint8_t *const direct = ps5w_mmap(NULL, 0x4000, PROT_READ | PROT_EXEC, MAP_SHARED, reading, 0);
   ps5w_live(&stats);
   CHECK(direct != MAP_FAILED && stats.shared_objects == 1 && !memcmp(direct, image, 0x4000), "a shared view mapped executable is of an object");
   CHECK(ps5w_munmap(direct, 0x4000) == 0 && ps5w_munmap(image, 0x8000) == 0 && ps5w_munmap(kept, 0x8000) == 0, "release them");
   ps5w_live(&stats);
   CHECK(!stats.shared_objects && !stats.direct_bytes, "an object of a read-only file goes without being written back");
   close(reading);
   close(file);
   balance("after shared sections");
}

/* A section with no file: the server declares its descriptor, and the memory
 * is the section. */
static void
declared_sections(void)
{
   typedef int (*function)(void);
   static const uint8_t code[] = {0xb8, 42, 0, 0, 0, 0xc3};
   struct ps5w_stats stats;
   char path[] = "/tmp/ps5-mman-section-XXXXXX";
   const int file = mkstemp(path);
   unlink(path);
   CHECK(file >= 0 && ftruncate(file, 0x30000) == 0 && ps5w_section(file) == 0, "a file declared a section");
   ps5w_live(&stats);
   CHECK(!stats.shared_objects && !stats.direct_bytes, "it has no memory before its first view");

   uint8_t *const first = ps5w_mmap(NULL, 0x30000, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
   ps5w_live(&stats);
   CHECK(first != MAP_FAILED && stats.shared_objects == 1 && stats.direct_bytes == 0x30000 && first[0] == 0 && first[0x2ffff] == 0,
         "its first view, though plain, is of zeroed direct memory");
   memcpy(first + 0x10000, code, sizeof(code));
   first[0x20000] = 7;
   uint8_t *const second = ps5w_mmap(NULL, 0x10000, PROT_READ | PROT_EXEC, MAP_SHARED, file, 0x10000);
   CHECK(second != MAP_FAILED && ((function)second)() == 42, "a second view runs what the first wrote");
   uint8_t check = 1;
   CHECK(pread(file, &check, 1, 0x20000) == 1 && check == 0, "the file is not written through the views");

   uint8_t *const copy = ps5w_mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE, file, 0x20000);
   CHECK(copy != MAP_FAILED && copy[0] == 7 && ps5w_query(copy, NULL) == PS5W_COMMITTED, "a private view is a copy of the section's memory");
   if (copy != MAP_FAILED)
      copy[0] = 8;
   CHECK(first[0x20000] == 7, "and writing to it leaves the section alone");
   CHECK(copy == MAP_FAILED || ps5w_munmap(copy, 0x10000) == 0, "release the copy");

   CHECK(ps5w_munmap(first, 0x30000) == 0 && ps5w_munmap(second, 0x10000) == 0, "unmap every view");
   ps5w_live(&stats);
   CHECK(stats.shared_objects == 1 && stats.direct_bytes == 0x30000, "an open section keeps its memory with no view");
   uint8_t *const again = ps5w_mmap(NULL, 0x30000, PROT_READ, MAP_SHARED, file, 0);
   CHECK(again != MAP_FAILED && again[0x20000] == 7 && !memcmp(again + 0x10000, code, sizeof(code)), "and a later view finds what was put there");
   ps5w_section_closed(file);
   ps5w_live(&stats);
   CHECK(stats.shared_objects == 1 && again[0x20000] == 7, "closed with a view left, it stays for the view");
   CHECK(ps5w_munmap(again, 0x30000) == 0, "unmap that view");
   ps5w_live(&stats);
   CHECK(!stats.shared_objects && !stats.direct_bytes, "and then the memory goes");
   CHECK(pread(file, &check, 1, 0x20000) == 1 && check == 0, "nothing was written to the file at any point");
   close(file);

   /* closed with no view: gone at once; and a section never mapped costs nothing */
   char other[] = "/tmp/ps5-mman-section-XXXXXX";
   const int next = mkstemp(other);
   unlink(other);
   CHECK(next >= 0 && ftruncate(next, 0x10000) == 0 && ps5w_section(next) == 0, "another section");
   uint8_t *const only = ps5w_mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, next, 0);
   CHECK(only != MAP_FAILED && ps5w_munmap(only, 0x10000) == 0, "mapped and unmapped");
   ps5w_section_closed(next);
   ps5w_live(&stats);
   CHECK(!stats.shared_objects && !stats.direct_bytes, "closing a section with no view releases it");
   const int never = mkstemp(strcpy(other, "/tmp/ps5-mman-section-XXXXXX"));
   unlink(other);
   CHECK(never >= 0 && ftruncate(never, 0x10000) == 0 && ps5w_section(never) == 0, "a third, never mapped");
   ps5w_section_closed(never);
   close(never);
   close(next);
   balance("after declared sections");
}

int
main(void)
{
   ps5w_init(NULL); /* on the host: the model */
   scripted();
   shared_sections();
   declared_sections();
   for (unsigned seed = 1; seed <= 8 && failed <= 20; seed++)
      randomised(seed, 4000);
   struct ps5w_stats stats;
   ps5w_live(&stats);
   CHECK(ps5w_model_held == 0 && stats.direct_bytes == 0, "direct memory is all returned");
   CHECK(ps5w_model_faults == 0, "%u releases or maps of direct memory the model did not hold", ps5w_model_faults);
   printf("%u checks, %u failed\n", checks, failed);
   return failed ? 1 : 0;
}
