/* A model of the console's kernel on Linux mmap, for ps5_mman.c on the host. MIT license.
 *
 * A reservation is a PROT_NONE mapping. Direct memory is a pool of unit numbers
 * handed out first fit (so released memory comes round again), and a direct
 * mapping is private anonymous memory filled with a pattern, because the
 * console does not clear what it gives back. The mapping is private so that a
 * Wine that forks to start a child process keeps working: the console has no
 * fork, and the layer never maps one piece of direct memory twice. Placements
 * with no usable hint go where the console puts them, under 1 TiB. The model
 * counts a release of memory it did not hand out, and a map of memory not
 * held, in ps5w_model_faults.
 *
 * It serves the host unit test and a Linux build of Wine that runs with the
 * console's memory rules (WINE_PS5_MEMORY_MODEL). It establishes this layer's
 * logic and Wine's behaviour on it, nothing about the console itself.
 */
#ifndef __PROSPERO__
#define _GNU_SOURCE
#include "ps5_mman.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define POOL_BYTES ((int64_t)12 << 30) /* the console's direct pool */
static uint8_t pool_unit[POOL_BYTES / PS5W_UNIT]; /* 1: allocated, 2: and mapped as an alias since */
static size_t pool_search;                           /* no free unit lies below this one */
int64_t ps5w_model_held;
unsigned ps5w_model_faults;

static int
model_reserve(void **address, size_t bytes, int fixed)
{
   const int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
   void *got = MAP_FAILED;
   if (fixed)
      got = mmap(*address, bytes, PROT_NONE, flags | MAP_FIXED, -1, 0);
   else if (*address)
      got = mmap(*address, bytes, PROT_NONE, flags | MAP_FIXED_NOREPLACE, -1, 0);
   /* the console places a taken or absent hint elsewhere, on a unit, under 1 TiB */
   for (uintptr_t at = 0x5000000000ull; got == MAP_FAILED && !fixed && at < 0x6000000000ull; at += (bytes + PS5W_UNIT - 1) & ~(PS5W_UNIT - 1))
      got = mmap((void *)at, bytes, PROT_NONE, flags | MAP_FIXED_NOREPLACE, -1, 0);
   if (got == MAP_FAILED)
      return -1;
   *address = got;
   return 0;
}

static int
model_allocate(size_t bytes, int64_t *start)
{
   /* first fit, so released memory is handed out again, as the console does */
   const size_t units = bytes / PS5W_UNIT, total = POOL_BYTES / PS5W_UNIT;
   size_t first = 0, run = 0;
   while (pool_search < total && pool_unit[pool_search])
      pool_search++;
   for (size_t unit = pool_search; unit < total && run < units; unit++) {
      run = pool_unit[unit] ? 0 : run + 1;
      first = unit + 1 - run;
   }
   if (bytes % PS5W_UNIT || !units || run < units)
      return -1;
   *start = (int64_t)(first * PS5W_UNIT);
   ps5w_model_held += (int64_t)bytes;
   for (size_t unit = 0; unit < units; unit++)
      pool_unit[first + unit] = 1;
   return 0;
}

static int
model_release(int64_t start, size_t bytes)
{
   if (start < 0 || start % (int64_t)PS5W_UNIT || bytes % PS5W_UNIT)
      return -1;
   for (size_t unit = 0; unit < bytes / PS5W_UNIT; unit++) {
      ps5w_model_faults += !pool_unit[(size_t)start / PS5W_UNIT + unit];
      pool_unit[(size_t)start / PS5W_UNIT + unit] = 0;
   }
   ps5w_model_held -= (int64_t)bytes;
   if ((size_t)start / PS5W_UNIT < pool_search)
      pool_search = (size_t)start / PS5W_UNIT;
   return 0;
}

static int
model_map(void *address, size_t bytes, int64_t start)
{
   ps5w_model_faults += !pool_unit[(size_t)start / PS5W_UNIT];
   if (mmap(address, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != address)
      return -1;
   memset(address, 0x5a, bytes);
   return 0;
}

static int
model_protect(void *address, size_t bytes, int prot)
{
   return mprotect(address, bytes, prot);
}

static int
model_unmap(void *address, size_t bytes)
{
   return munmap(address, bytes);
}

/* Direct memory mapped at more than one address: a shared mapping of one file
 * that stands for the pool. Only the objects the layer keeps for executable
 * views of shared sections are mapped this way, and each process has its own
 * pool: a section two processes share is not one memory here, as it would be
 * on the console, where there is one process. */
static int
model_alias(void *address, size_t bytes, int64_t start)
{
   static int pool = -1;
   if (pool < 0) {
      pool = memfd_create("ps5-direct-memory-model", MFD_CLOEXEC);
      if (pool < 0 || ftruncate(pool, POOL_BYTES) != 0)
         return -1;
   }
   for (size_t unit = (size_t)start / PS5W_UNIT; unit <= ((size_t)start + bytes - 1) / PS5W_UNIT; unit++) {
      static uint8_t dirt[PS5W_UNIT];
      ps5w_model_faults += !pool_unit[unit];
      if (pool_unit[unit] != 1)
         continue;
      /* its first mapping since it was allocated: the console does not clear it */
      memset(dirt, 0x5a, sizeof(dirt));
      if (pwrite(pool, dirt, sizeof(dirt), (off_t)(unit * PS5W_UNIT)) != (ssize_t)sizeof(dirt))
         return -1;
      pool_unit[unit] = 2;
   }
   return mmap(address, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, pool, start) == address ? 0 : -1;
}

static const struct ps5w_kernel model = {model_reserve, model_allocate, model_release, model_map,
                                         model_protect, model_unmap,    model_alias};

const struct ps5w_kernel *
ps5w_host_model(void)
{
   return &model;
}
#endif /* __PROSPERO__ */
