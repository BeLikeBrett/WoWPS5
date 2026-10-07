/* BSD mmap semantics for Wine on the PS5's memory model (ps5_mman.h). MIT license. */
#include "ps5_mman.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef MAP_EXCL
#define EXCLUSIVE(flags) (((flags) & (MAP_FIXED | MAP_EXCL)) == (MAP_FIXED | MAP_EXCL))
#define KERNEL_FLAGS(flags) ((flags) & ~MAP_EXCL)
#else /* the host test's Linux: MAP_FIXED_NOREPLACE is the same request */
#define EXCLUSIVE(flags) (((flags) & MAP_FIXED_NOREPLACE) != 0)
#define KERNEL_FLAGS(flags) ((flags) & ~MAP_FIXED_NOREPLACE)
#endif
#define FIXED(flags) (((flags) & MAP_FIXED) || EXCLUSIVE(flags))

#define ACCESS (PROT_READ | PROT_WRITE | PROT_EXEC)
#define READ_WRITE (PROT_READ | PROT_WRITE)
#define KIND(entry) ((entry) & 3)
#define PROTECTION(entry) ((entry) >> 2 & ACCESS)
#define ENTRY(kind, prot) ((uint8_t)((kind) | ((prot) & ACCESS) << 2))
/* a file page that is a view of a shared object's direct memory */
#define ALIAS 0x20
/* One direct allocation backs at most this many whole units at once. */
#define RUN_UNITS 4096

static const struct ps5w_kernel *kernel;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
/* Per GiB of address space, made at first use: a byte a page, and each
 * unit's direct memory (-1: none). */
static uint8_t *pages[PS5W_ADDRESS_LIMIT >> 30];
static int64_t *directs[PS5W_ADDRESS_LIMIT >> 30];
static struct ps5w_stats live;

/* A file whose shared views live in direct memory, because one of them is
 * executable. */
struct object {
   dev_t device;
   ino_t inode;
   int fd;          /* a descriptor of the file, kept to write the memory back */
   int64_t start;   /* direct memory */
   size_t bytes;    /* whole units */
   size_t file_bytes;
   unsigned views;
   bool written;    /* a view has been writable: the file gets the memory back */
   bool section;    /* declared by ps5w_section: the file is a name only, the memory is the contents */
   bool closed;     /* such a section has been closed: the memory goes with the last view */
};
/* A file mapping the layer made, of the kernel's own or of an object. */
struct view {
   uintptr_t low, high;
   dev_t device;
   ino_t inode;
   off_t offset;    /* of low in the file */
   int object;      /* index in objects, or -1 */
   int fd;          /* writable shared views of the file itself: a descriptor, should it need an object later */
   bool shared;
};
static struct object *objects;
static struct view *views;
static size_t object_count, view_count, view_room;

static uint8_t *
page_entry(uintptr_t address, bool create)
{
   const size_t gib = address >> 30;
   if (!pages[gib]) {
      if (!create)
         return NULL;
      pages[gib] = calloc((1u << 30) / PS5W_PAGE, 1);
      if (!pages[gib])
         return NULL;
   }
   return &pages[gib][(address & ((1u << 30) - 1)) / PS5W_PAGE];
}

static int64_t *
unit_entry(uintptr_t address, bool create)
{
   const size_t gib = address >> 30, count = (1u << 30) / PS5W_UNIT;
   if (!directs[gib]) {
      if (!create)
         return NULL;
      directs[gib] = malloc(count * sizeof(int64_t));
      if (!directs[gib])
         return NULL;
      for (size_t unit = 0; unit < count; unit++)
         directs[gib][unit] = -1;
   }
   return &directs[gib][(address & ((1u << 30) - 1)) / PS5W_UNIT];
}

static int
kind_at(uintptr_t address)
{
   const uint8_t *const entry = page_entry(address, false);
   return entry ? KIND(*entry) : PS5W_FREE;
}

static bool
mark(uintptr_t low, uintptr_t high, int kind, int prot)
{
   for (uintptr_t at = low; at < high; at += PS5W_PAGE) {
      uint8_t *const entry = page_entry(at, kind != PS5W_FREE);
      if (!entry) {
         if (kind != PS5W_FREE)
            return false;
         continue;
      }
      uint64_t *const counters[] = {NULL, &live.reserved_pages, &live.committed_pages, &live.file_pages};
      if (counters[KIND(*entry)])
         --*counters[KIND(*entry)];
      if (counters[kind])
         ++*counters[kind];
      *entry = ENTRY(kind, prot);
   }
   return true;
}

static void
flag_alias(uintptr_t low, uintptr_t high)
{
   for (uintptr_t at = low; at < high; at += PS5W_PAGE) {
      uint8_t *const entry = page_entry(at, false);
      if (entry)
         *entry |= ALIAS;
   }
}

static bool
is_alias(uintptr_t address)
{
   const uint8_t *const entry = page_entry(address, false);
   return entry && KIND(*entry) == PS5W_FILE && (*entry & ALIAS);
}

/* Copies between an object's memory and its file, through a mapping of the
 * whole object made for the moment. */
static int
object_transfer(const struct object *object, bool to_file)
{
   void *scratch = NULL;
   if (kernel->reserve(&scratch, object->bytes, 0) != 0)
      return ENOMEM;
   int error = 0;
   if (kernel->alias(scratch, object->bytes, object->start) != 0)
      error = ENOMEM;
   else if (object->section)
      memset(scratch, 0, object->bytes); /* direct memory comes back dirty */
   else if (to_file) {
      for (size_t done = 0; done < object->file_bytes && !error;) {
         const ssize_t wrote = pwrite(object->fd, (char *)scratch + done, object->file_bytes - done, (off_t)done);
         if (wrote <= 0)
            error = wrote ? errno : EIO;
         else
            done += (size_t)wrote;
      }
   } else {
      size_t done = 0;
      while (done < object->file_bytes) {
         const ssize_t got = pread(object->fd, (char *)scratch + done, object->file_bytes - done, (off_t)done);
         if (got <= 0)
            break;
         done += (size_t)got;
      }
      memset((char *)scratch + done, 0, object->bytes - done); /* direct memory comes back dirty */
   }
   kernel->unmap(scratch, object->bytes);
   return error;
}

/* The last view of an object has gone: the file gets the memory's contents
 * back, and the memory is released. An object whose file cannot be written
 * keeps its memory, so that a later view still finds what was put there, and
 * so does a section that is still open: its memory is all there is of it. */
static void
object_retire(int index)
{
   struct object *const object = &objects[index];
   if (object->views || (object->section && !object->closed))
      return;
   if (!object->section && object->written && object_transfer(object, true) != 0)
      return;
   if (object->start >= 0) {
      kernel->release(object->start, object->bytes);
      live.direct_bytes -= object->bytes;
      live.shared_objects--;
   }
   close(object->fd);
   for (size_t v = 0; v < view_count; v++) /* none refers to it; later ones move down */
      if (views[v].object > index)
         views[v].object--;
   memmove(object, object + 1, (object_count - (size_t)index - 1) * sizeof(*object));
   object_count--;
}

static int back(uintptr_t low, uintptr_t high);

static bool
view_add(uintptr_t low, uintptr_t high, const struct stat *file, off_t offset, bool shared, int object, int fd)
{
   if (view_count == view_room) {
      const size_t room = view_room ? view_room * 2 : 64;
      struct view *const grown = realloc(views, room * sizeof(*views));
      if (!grown)
         return false;
      views = grown;
      view_room = room;
   }
   views[view_count++] = (struct view){low, high, file->st_dev, file->st_ino, offset, object, fd, shared};
   return true;
}

/* Whatever file views lie in [low, high) are no longer there: the range has
 * been unmapped or mapped over. */
static void
views_forget(uintptr_t low, uintptr_t high)
{
   for (size_t v = 0; v < view_count;) {
      struct view *const view = &views[v];
      if (view->high <= low || view->low >= high) {
         v++;
         continue;
      }
      if (view->low < low && view->high > high) { /* a hole: the part above it is a view of its own */
         const struct view above = *view;
         const struct stat file = {.st_dev = above.device, .st_ino = above.inode};
         view->high = low; /* view_add may move the table: nothing reads view after it */
         if (view_add(high, above.high, &file, above.offset + (off_t)(high - above.low), above.shared, above.object,
                      above.fd >= 0 ? dup(above.fd) : -1) && above.object >= 0)
            objects[above.object].views++;
         v++;
      } else if (view->low < low) {
         view->high = low;
         v++;
      } else if (view->high > high) {
         view->offset += (off_t)(high - view->low);
         view->low = high;
         v++;
      } else {
         const int object = view->object;
         if (view->fd >= 0)
            close(view->fd);
         *view = views[--view_count];
         if (object >= 0 && --objects[object].views == 0)
            object_retire(object);
      }
   }
}

/* The object for a file, made on first need: its memory is loaded from the
 * file, and the shared views of the file the layer already made are moved
 * onto it, so that every view shows the same memory. */
static int
object_for(int fd, const struct stat *file)
{
   struct object *object = NULL;
   for (size_t o = 0; o < object_count && !object; o++)
      if (objects[o].device == file->st_dev && objects[o].inode == file->st_ino)
         object = &objects[o];
   if (object && object->start >= 0)
      return (int)(object - objects);
   const bool declared = object != NULL; /* a section with no memory yet: this is its first view */
   if (!declared) {
      if (fd < 0)
         return -1;
      struct object *const grown = realloc(objects, (object_count + 1) * sizeof(*objects));
      if (!grown)
         return -1;
      objects = grown;
      object = &objects[object_count];
      *object = (struct object){.device = file->st_dev, .inode = file->st_ino, .fd = dup(fd), .start = -1};
   }
   object->bytes = ((size_t)file->st_size + PS5W_UNIT - 1) & ~(PS5W_UNIT - 1);
   object->file_bytes = (size_t)file->st_size;
   if (object->fd < 0 || !object->bytes)
      goto failed;
   if (kernel->allocate(object->bytes, &object->start) != 0)
      goto failed;
   if (object_transfer(object, false) != 0) {
      kernel->release(object->start, object->bytes);
      goto failed;
   }
   live.direct_bytes += object->bytes;
   live.allocations++;
   live.shared_objects++;
   const int index = declared ? (int)(object - objects) : (int)object_count++;
   if (declared)
      return index; /* every view of a section is of its object from the start: none to move */
   for (size_t v = 0; v < view_count; v++) {
      struct view *const view = &views[v];
      if (!view->shared || view->object >= 0 || view->device != file->st_dev || view->inode != file->st_ino)
         continue;
      const uint8_t *const entry = page_entry(view->low, false);
      const int prot = entry ? PROTECTION(*entry) : PROT_READ;
      if ((size_t)view->offset + (view->high - view->low) > objects[index].bytes ||
          kernel->alias((void *)view->low, view->high - view->low, objects[index].start + view->offset) != 0)
         continue; /* stays a view of the file itself */
      if (prot != READ_WRITE)
         kernel->protect((void *)view->low, view->high - view->low, prot);
      flag_alias(view->low, view->high);
      view->object = index;
      objects[index].views++;
      objects[index].written |= (prot & PROT_WRITE) != 0;
      if (view->fd >= 0) /* the object has its own */
         close(view->fd);
      view->fd = -1;
   }
   return index;

failed:
   object->start = -1;
   if (!declared && object->fd >= 0)
      close(object->fd);
   return -1;
}

/* The server's sections with no file: see ps5_mman.h. */
int
ps5w_section(int fd)
{
   struct stat file;
   if (fstat(fd, &file) != 0)
      return -1;
   int result = -1;
   pthread_mutex_lock(&lock);
   struct object *const grown = realloc(objects, (object_count + 1) * sizeof(*objects));
   if (grown) {
      objects = grown;
      objects[object_count] = (struct object){.device = file.st_dev, .inode = file.st_ino, .fd = dup(fd), .start = -1, .section = true};
      if (objects[object_count].fd >= 0) {
         object_count++;
         result = 0;
      }
   }
   pthread_mutex_unlock(&lock);
   return result;
}

/* Before the process asks to be closed: what its views of files have changed
 * goes to the files now, in the process's own time. Left to the kernel it is
 * written while the console waits for the process to be gone, and the console
 * does not wait long. */
void
ps5w_flush(void)
{
   for (int attempt = 0; attempt < 50 && pthread_mutex_trylock(&lock) != 0; attempt++) {
      if (attempt == 49)
         return; /* held by a thread that will not let go: nothing to be done */
      usleep(20000);
   }
   for (size_t v = 0; v < view_count; v++)
      if (views[v].shared && views[v].object < 0 && views[v].fd >= 0)
         msync((void *)views[v].low, views[v].high - views[v].low, MS_SYNC);
   pthread_mutex_unlock(&lock);
}

void
ps5w_section_closed(int fd)
{
   struct stat file;
   if (fstat(fd, &file) != 0)
      return;
   pthread_mutex_lock(&lock);
   for (size_t o = 0; o < object_count; o++)
      if (objects[o].section && objects[o].device == file.st_dev && objects[o].inode == file.st_ino) {
         objects[o].closed = true;
         object_retire((int)o);
         break;
      }
   pthread_mutex_unlock(&lock);
}

/* A private file view made executable becomes an anonymous copy of itself. */
static int
privatise(uintptr_t low, uintptr_t high, int prot)
{
   const size_t bytes = high - low;
   void *const copy = malloc(bytes);
   if (!copy)
      return ENOMEM;
   int error = 0;
   if (mprotect((void *)low, bytes, PROT_READ) != 0)
      error = errno;
   else {
      memcpy(copy, (void *)low, bytes);
      views_forget(low, high);
      mark(low, high, PS5W_RESERVED, 0);
      if (!(error = back(low, high))) {
         memcpy((void *)low, copy, bytes);
         if (prot != READ_WRITE && kernel->protect((void *)low, bytes, prot) != 0)
            error = EACCES;
         mark(low, high, PS5W_COMMITTED, prot);
      }
   }
   free(copy);
   return error;
}


/* A unit whose last committed page has gone gives its direct memory back.
 * Units that lie together in the pool go back in one call: a release costs
 * about 33 us on the console whatever its size. */
static void
settle(uintptr_t low, uintptr_t high)
{
   int64_t pending = -1;
   size_t pending_bytes = 0;
   for (uintptr_t unit = low & ~(PS5W_UNIT - 1); unit < high; unit += PS5W_UNIT) {
      int64_t *const start = unit_entry(unit, false);
      if (!start) { /* no unit of this GiB was ever backed */
         unit = (unit | ((1u << 30) - 1)) - PS5W_UNIT + 1;
         continue;
      }
      if (*start < 0)
         continue;
      bool used = false;
      for (uintptr_t at = unit; at < unit + PS5W_UNIT; at += PS5W_PAGE)
         used |= kind_at(at) == PS5W_COMMITTED;
      if (used)
         continue;
      if (pending >= 0 && *start != pending + (int64_t)pending_bytes) {
         kernel->release(pending, pending_bytes);
         pending = -1;
      }
      if (pending < 0) {
         pending = *start;
         pending_bytes = 0;
      }
      pending_bytes += PS5W_UNIT;
      live.direct_bytes -= PS5W_UNIT;
      *start = -1;
   }
   if (pending >= 0)
      kernel->release(pending, pending_bytes);
}

/* Maps direct memory, zeroed and read-write, under the pages of [low, high),
 * none of which is committed, and marks them. */
static int
back(uintptr_t low, uintptr_t high)
{
   for (uintptr_t at = low; at < high;) {
      const uintptr_t unit = at & ~(PS5W_UNIT - 1);
      int64_t *const start = unit_entry(unit, true);
      if (!start || !page_entry(at, true))
         return ENOMEM;
      /* Whole units with no direct memory yet: one allocation, one mapping. */
      size_t run = 0;
      if (at == unit)
         while (run < RUN_UNITS && unit + (run + 1) * PS5W_UNIT <= high) {
            const int64_t *const next = unit_entry(unit + run * PS5W_UNIT, true);
            if (!next || *next >= 0 || !page_entry(unit + run * PS5W_UNIT, true))
               break;
            run++;
         }
      if (run) {
         int64_t first = -1;
         int result = kernel->allocate(run * PS5W_UNIT, &first);
         if (result != 0 && run > 1) { /* the pool may still hold single units */
            run = 1;
            result = kernel->allocate(PS5W_UNIT, &first);
         }
         if (result != 0)
            return ENOMEM;
         if (kernel->map((void *)unit, run * PS5W_UNIT, first) != 0) {
            kernel->release(first, run * PS5W_UNIT);
            return ENOMEM;
         }
         for (size_t index = 0; index < run; index++)
            *unit_entry(unit + index * PS5W_UNIT, false) = first + (int64_t)(index * PS5W_UNIT);
         live.direct_bytes += run * PS5W_UNIT;
         live.allocations++;
         /* Direct memory comes back with what it held before. */
         memset((void *)unit, 0, run * PS5W_UNIT);
         mark(unit, unit + run * PS5W_UNIT, PS5W_COMMITTED, READ_WRITE);
         at = unit + run * PS5W_UNIT;
         continue;
      }
      /* Part of a unit: its pages are mapped from the unit's own memory. */
      const bool fresh = *start < 0;
      if (fresh) {
         if (kernel->allocate(PS5W_UNIT, start) != 0) {
            *start = -1;
            return ENOMEM;
         }
         live.direct_bytes += PS5W_UNIT;
         live.allocations++;
      }
      const uintptr_t end = high < unit + PS5W_UNIT ? high : unit + PS5W_UNIT;
      if (kernel->map((void *)at, end - at, *start + (int64_t)(at - unit)) != 0) {
         settle(unit, unit + PS5W_UNIT);
         return ENOMEM;
      }
      memset((void *)at, 0, end - at);
      mark(at, end, PS5W_COMMITTED, READ_WRITE);
      at = end;
   }
   return 0;
}

/* The pages of [low, high) become committed with prot. A page committed
 * already keeps its contents, unless fresh memory was asked for. */
static int
commit(uintptr_t low, uintptr_t high, int prot, bool fresh)
{
   for (uintptr_t at = low; at < high;) {
      const bool backed = kind_at(at) == PS5W_COMMITTED;
      uintptr_t end = at + PS5W_PAGE;
      while (end < high && (kind_at(end) == PS5W_COMMITTED) == backed)
         end += PS5W_PAGE;
      if (!backed) {
         const int result = back(at, end);
         if (result != 0)
            return result;
      } else if (fresh) {
         if (kernel->protect((void *)at, end - at, READ_WRITE) != 0)
            return ENOMEM;
         memset((void *)at, 0, end - at);
      }
      if ((!backed || fresh) ? prot != READ_WRITE : true)
         if (kernel->protect((void *)at, end - at, prot) != 0)
            return EACCES;
      mark(at, end, PS5W_COMMITTED, prot);
      at = end;
   }
   return 0;
}

static bool
range(void *address, size_t bytes, uintptr_t *low, uintptr_t *high)
{
   const uintptr_t at = (uintptr_t)address;
   if (!bytes || at >= PS5W_ADDRESS_LIMIT || bytes > PS5W_ADDRESS_LIMIT - at)
      return false;
   *low = at & ~(PS5W_PAGE - 1);
   *high = (at + bytes + PS5W_PAGE - 1) & ~(PS5W_PAGE - 1);
   return *high <= PS5W_ADDRESS_LIMIT;
}

void *
ps5w_mmap(void *address, size_t bytes, int prot, int flags, int fd, off_t offset)
{
   const bool anonymous = (flags & MAP_ANON) != 0, fixed = FIXED(flags), exclusive = EXCLUSIVE(flags);
   const size_t size = (bytes + PS5W_PAGE - 1) & ~(PS5W_PAGE - 1);
   uintptr_t low = (uintptr_t)address, high;
   if (!bytes || size < bytes || (fixed && (low % PS5W_PAGE || !range(address, size, &low, &high)))) {
      errno = EINVAL;
      return MAP_FAILED;
   }
   struct stat file = {0};
   const bool shared = !anonymous && (flags & MAP_SHARED);
   if (!anonymous && fstat(fd, &file) != 0)
      return MAP_FAILED;
   /* File pages never run on the console. A private view that is to run is
    * refused below, and Wine then reads the file instead; a shared one becomes
    * a view of an object. */
   if (shared && (prot & PROT_WRITE) && (fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDONLY) {
      errno = EACCES;
      return MAP_FAILED;
   }
   int error = 0;
   bool placed = false; /* this call made the reservation, and undoes it on failure */
   pthread_mutex_lock(&lock);
   if (exclusive)
      for (uintptr_t at = low; at < low + size && !error; at += PS5W_PAGE)
         if (kind_at(at) != PS5W_FREE)
            error = EEXIST;
   if (!error && (!fixed || exclusive)) {
      void *at = address;
      if (kernel->reserve(&at, size, 0) != 0)
         error = ENOMEM;
      else if ((exclusive && at != address) || !range(at, size, &low, &high) || !mark(low, high, PS5W_RESERVED, 0)) {
         kernel->unmap(at, size);
         error = exclusive ? EEXIST : ENOMEM;
      } else
         placed = true;
   }
   high = low + size;
   if (!error && !placed)
      views_forget(low, high); /* what was mapped there is being replaced */
   if (!error && anonymous) {
      if (prot & ACCESS)
         error = commit(low, high, prot, true);
      else if (!placed) {
         void *at = (void *)low;
         if (kernel->reserve(&at, size, 1) != 0 || !mark(low, high, PS5W_RESERVED, 0))
            error = ENOMEM;
         settle(low, high);
      }
   } else if (!error) {
      /* a shared view that runs, or of a file that has an object already */
      int object = shared ? object_for((prot & PROT_EXEC) ? fd : -1, &file) : -1;
      if (object < 0 && shared && (prot & PROT_EXEC))
         error = ENOMEM;
      /* A section's file is empty: a private view is a copy of its memory. */
      bool copied = false;
      for (size_t o = 0; !shared && !error && o < object_count; o++)
         if (objects[o].section && objects[o].device == file.st_dev && objects[o].inode == file.st_ino) {
            const int section = object_for(-1, &file);
            void *scratch = NULL;
            if (section < 0 || offset < 0 || (size_t)offset + size > objects[section].bytes)
               error = EINVAL;
            else if ((error = commit(low, high, READ_WRITE, true)) == 0) {
               if (kernel->reserve(&scratch, size, 0) != 0 || kernel->alias(scratch, size, objects[section].start + offset) != 0)
                  error = ENOMEM;
               else
                  memcpy((void *)low, scratch, size);
               if (scratch)
                  kernel->unmap(scratch, size);
               if (!error && prot != READ_WRITE && kernel->protect((void *)low, size, prot & ACCESS) != 0)
                  error = EACCES;
               if (!error)
                  mark(low, high, PS5W_COMMITTED, prot);
            }
            copied = true;
            break;
         }
      if (copied) {
         /* done, or failed */
      } else if (!error && !shared && (prot & PROT_EXEC)) {
         /* File pages never run on the console: Wine reads the file instead. */
         error = ENODEV;
      } else if (!error && object >= 0) {
         if (offset < 0 || (size_t)offset + size > objects[object].bytes)
            error = EINVAL;
         else if (kernel->alias((void *)low, size, objects[object].start + offset) != 0)
            error = ENOMEM;
         else {
            if (prot != READ_WRITE)
               kernel->protect((void *)low, size, prot);
            mark(low, high, PS5W_FILE, prot);
            flag_alias(low, high);
            if (view_add(low, high, &file, offset, true, object, -1))
               objects[object].views++;
            objects[object].written |= (prot & PROT_WRITE) != 0;
            settle(low, high);
         }
         if (error && !objects[object].views)
            object_retire(object);
      } else if (!error) {
         if (mmap((void *)low, bytes, prot, KERNEL_FLAGS(flags) | MAP_FIXED, fd, offset) == MAP_FAILED)
            error = errno;
         else {
            mark(low, high, PS5W_FILE, prot);
            view_add(low, high, &file, offset, shared, -1, shared && (prot & PROT_WRITE) ? dup(fd) : -1);
            settle(low, high);
         }
      }
   }
   if (error && placed) {
      kernel->unmap((void *)low, size);
      mark(low, high, PS5W_FREE, 0);
      settle(low, high);
   }
   pthread_mutex_unlock(&lock);
   if (error) {
      errno = error;
      return MAP_FAILED;
   }
   return (void *)low;
}

int
ps5w_munmap(void *address, size_t bytes)
{
   uintptr_t low, high;
   if ((uintptr_t)address % PS5W_PAGE || !range(address, bytes, &low, &high)) {
      errno = EINVAL;
      return -1;
   }
   int error = 0;
   pthread_mutex_lock(&lock);
   for (uintptr_t at = low; at < high;) {
      const int kind = kind_at(at);
      const bool alias = is_alias(at);
      uintptr_t end = at + PS5W_PAGE;
      while (end < high && kind_at(end) == kind && is_alias(end) == alias)
         end += PS5W_PAGE;
      if (kind == PS5W_FILE && !alias ? munmap((void *)at, end - at) != 0
                                      : kind != PS5W_FREE && kernel->unmap((void *)at, end - at) != 0)
         error = EINVAL;
      at = end;
   }
   mark(low, high, PS5W_FREE, 0);
   views_forget(low, high); /* after the unmap: an object writes itself back when its last view goes */
   settle(low, high);
   pthread_mutex_unlock(&lock);
   if (error)
      errno = error;
   return error ? -1 : 0;
}

int
ps5w_mprotect(void *address, size_t bytes, int prot)
{
   uintptr_t low, high;
   if (!range(address, bytes, &low, &high)) {
      errno = EINVAL;
      return -1;
   }
   int error = 0;
   pthread_mutex_lock(&lock);
   for (uintptr_t at = low; at < high && !error;) {
      const int kind = kind_at(at);
      const bool alias = is_alias(at);
      uintptr_t end = at + PS5W_PAGE;
      while (end < high && kind_at(end) == kind && is_alias(end) == alias)
         end += PS5W_PAGE;
      switch (kind) {
      case PS5W_FREE:
         error = ENOMEM;
         break;
      case PS5W_RESERVED: /* first access to reserved memory commits it */
         if (prot & ACCESS)
            error = commit(at, end, prot, false);
         break;
      case PS5W_COMMITTED:
         if (kernel->protect((void *)at, end - at, prot & ACCESS) != 0)
            error = EACCES;
         else
            mark(at, end, PS5W_COMMITTED, prot);
         break;
      case PS5W_FILE:
         if (!alias && (prot & PROT_EXEC)) {
            /* File pages cannot run. A shared view moves onto an object with
             * the file's other shared views when one of them can be written:
             * the program is going to run what it writes there. Any other
             * view becomes a copy of what the file holds now. */
            struct view *found = NULL;
            for (size_t v = 0; v < view_count && !found; v++)
               if (views[v].low <= at && at < views[v].high)
                  found = &views[v];
            if (found && found->high < end)
               end = found->high; /* a view at a time */
            int fd = -1;
            for (size_t v = 0; found && found->shared && v < view_count && fd < 0; v++)
               if (views[v].device == found->device && views[v].inode == found->inode)
                  fd = views[v].fd;
            struct stat file;
            if (fd < 0 || fstat(fd, &file) != 0 || object_for(fd, &file) < 0 || !is_alias(at)) {
               error = privatise(at, end, prot & ACCESS);
               break;
            }
         }
         if (is_alias(at)) {
            if (kernel->protect((void *)at, end - at, prot & ACCESS) != 0)
               error = EACCES;
            else {
               mark(at, end, PS5W_FILE, prot);
               flag_alias(at, end);
               if (prot & PROT_WRITE)
                  for (size_t v = 0; v < view_count; v++)
                     if (views[v].low <= at && at < views[v].high && views[v].object >= 0)
                        objects[views[v].object].written = true;
            }
         } else if (mprotect((void *)at, end - at, prot) != 0)
            error = errno;
         else
            mark(at, end, PS5W_FILE, prot);
         break;
      }
      at = end;
   }
   pthread_mutex_unlock(&lock);
   if (error)
      errno = error;
   return error ? -1 : 0;
}

int
ps5w_madvise(void *address, size_t bytes, int advice)
{
   uintptr_t low, high;
   if (advice != MADV_DONTNEED || !range(address, bytes, &low, &high))
      return 0;
   /* Anonymous pages read as zero afterwards, as they do where Wine was written. */
   pthread_mutex_lock(&lock);
   for (uintptr_t at = low; at < high; at += PS5W_PAGE) {
      const uint8_t *const entry = page_entry(at, false);
      if (!entry || KIND(*entry) != PS5W_COMMITTED)
         continue;
      const int prot = PROTECTION(*entry);
      if ((prot & READ_WRITE) == READ_WRITE || kernel->protect((void *)at, PS5W_PAGE, READ_WRITE) == 0) {
         memset((void *)at, 0, PS5W_PAGE);
         if ((prot & READ_WRITE) != READ_WRITE)
            kernel->protect((void *)at, PS5W_PAGE, prot);
      }
   }
   pthread_mutex_unlock(&lock);
   return 0;
}

int
ps5w_msync(void *address, size_t bytes, int flags)
{
   uintptr_t low, high;
   if (!range(address, bytes, &low, &high)) {
      errno = EINVAL;
      return -1;
   }
   int result = 0;
   pthread_mutex_lock(&lock);
   for (uintptr_t at = low; at < high;) {
      const int kind = kind_at(at);
      uintptr_t end = at + PS5W_PAGE;
      while (end < high && kind_at(end) == kind)
         end += PS5W_PAGE;
      if (kind == PS5W_FILE && !is_alias(at) && msync((void *)at, end - at, flags) != 0)
         result = -1; /* an object's file is written when its last view goes */
      at = end;
   }
   pthread_mutex_unlock(&lock);
   return result;
}

/* Direct memory is never paged out. */
int
ps5w_mlock(const void *address, size_t bytes)
{
   (void)address;
   (void)bytes;
   return 0;
}

int
ps5w_munlock(const void *address, size_t bytes)
{
   (void)address;
   (void)bytes;
   return 0;
}

enum ps5w_kind
ps5w_query(const void *address, int *prot)
{
   pthread_mutex_lock(&lock);
   const uint8_t *const entry = (uintptr_t)address < PS5W_ADDRESS_LIMIT ? page_entry((uintptr_t)address, false) : NULL;
   const uint8_t value = entry ? *entry : 0;
   pthread_mutex_unlock(&lock);
   if (prot)
      *prot = PROTECTION(value);
   return (enum ps5w_kind)KIND(value);
}

void
ps5w_live(struct ps5w_stats *stats)
{
   pthread_mutex_lock(&lock);
   *stats = live;
   pthread_mutex_unlock(&lock);
}

#ifdef __PROSPERO__
#include <ps5platform/kernel.h>

/* Where a mapping with no address goes: clear of the GPU window, the title
 * heap's reservation at 0x20_0000_0000 and the driver's at 0x40_0000_0000. */
#define ANYWHERE ((void *)0x5000000000ull)
#define GPU_WINDOW_LOW 0x200000000ull
#define GPU_WINDOW_HIGH 0x300000000ull

static int
console_reserve(void **address, size_t bytes, int fixed)
{
   void *at = *address ? *address : ANYWHERE;
   const size_t alignment = fixed || (uintptr_t)at % PS5W_UNIT ? PS5W_PAGE : PS5W_UNIT;
   int32_t result = sceKernelReserveVirtualRange(&at, bytes, fixed ? PS5_KERNEL_MAP_FIXED : 0, alignment);
   if (result == 0 && !fixed && (uintptr_t)at < GPU_WINDOW_HIGH && (uintptr_t)at + bytes > GPU_WINDOW_LOW) {
      sceKernelMunmap(at, bytes);
      at = ANYWHERE;
      result = sceKernelReserveVirtualRange(&at, bytes, 0, PS5W_UNIT);
   }
   if (result == 0)
      *address = at;
   return result;
}

static int
console_allocate(size_t bytes, int64_t *start)
{
   return sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes, PS5W_UNIT,
                                        PS5_KERNEL_DIRECT_TYPE_CPU, start);
}

static int
console_release(int64_t start, size_t bytes)
{
   return sceKernelReleaseDirectMemory(start, bytes);
}

static int
console_map(void *address, size_t bytes, int64_t start)
{
   void *at = address;
   /* Execute asked for at map time is refused: it comes with a later protect. */
   const int32_t result = sceKernelMapDirectMemory(&at, bytes, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE,
                                                   PS5_KERNEL_MAP_FIXED, start, PS5W_PAGE);
   if (result == 0 && at != address) {
      sceKernelMunmap(at, bytes);
      return -1;
   }
   return result;
}

static int
console_protect(void *address, size_t bytes, int prot)
{
   return sceKernelMprotect(address, bytes, prot & ACCESS); /* PROT_* are the kernel's own bits */
}

static int
console_unmap(void *address, size_t bytes)
{
   return sceKernelMunmap(address, bytes);
}

static const struct ps5w_kernel console = {console_reserve, console_allocate, console_release, console_map,
                                           console_protect, console_unmap,    console_map /* any mapping of direct memory is an alias */};
#endif

void
ps5w_init(const struct ps5w_kernel *chosen)
{
#ifdef __PROSPERO__
   kernel = chosen ? chosen : &console;
#else
   kernel = chosen ? chosen : ps5w_host_model();
#endif
}
