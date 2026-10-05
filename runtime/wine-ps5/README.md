# Wine for the PS5 title

Upstream Wine `455e3509b98a6919fd4ad1def4803e08c41c03b2` (11.19) plus what is here. Wine is LGPL-2.1-or-later; the patch is a derived work under the same licence.

| File | What |
|---|---|
| `wine-ps5.patch` | Every change to Wine's source, against the pinned revision. Guarded by `__PROSPERO__`, `WINE_INPROC_SERVER` or `WINE_PS5_MEMORY_MODEL`, so other builds are unchanged. |
| `ps5_mman.c`, `ps5_mman.h` | `mmap` semantics on reservations and direct memory. Copied into the Wine tree by `tools/wine-ps5/sync-memory-layer.sh`. |
| `ps5_mman_model.c` | A Linux model of the console's kernel, for the host test and the host build. |
| `ntdll-eh-frame.ld` | Unwind bounds for the standalone `ntdll.so` link check. |

What the patch changes, each for a measured reason (docs/technical-notes.md):

- **Server in the same process** (`server/`, `dlls/ntdll/unix/server.c`, `process.c`, `thread.c`): a title cannot start a second program.
- **Memory** (`virtual.c`): the layer above, 16 KiB host pages, nothing at or above 1 TiB, reserved areas clear of the title; decommitted 4 KiB pages that share a host page are cleared.
- **Syscall dispatcher pointer** (`include/wine/asm.h`, `signal_x86_64.c`): moved from 0x7ffe1000 to 0x7ffe4000, out of the shared-data host page. PE DLLs must be built with `-DWINE_PS5` or `-DWINE_PS5_MEMORY_MODEL`.
- **Thread registers** (`signal_x86_64.c`, `system.c`): GS base through `sysarch`; no `wrfsbase`/`wrgsbase` (they fault although CPUID lists them); the signal context's XSAVE image is 32 bytes after the SDK header's `mc_fpstate`.
- **Paths** (`loader.c`): a fixed installation root, `WINEPS5ROOT` or `/data/wowps5/wine`.
- **Output** (`debug.c`, `env.c`): diagnostics and the program's standard output go to klog; descriptor 2 is the null device in a title and cannot be replaced.

- **Unix libraries** (`virtual.c`): looked up in a table the title links, in place of `dlopen`.
- **Sections that run** (`virtual.c`, `server/mapping.c`, the memory layer): a shared view that is to run is mapped with execute permission from the start, which tells the layer to hold the file in direct memory; a section with no file is declared to the layer by the server and lives in direct memory altogether, its file left empty.
- **Sealed code pages** (`virtual.c`, `unix_private.h`): a host page of an image or executable section stays inaccessible while one of its four Windows pages is, and an access the program is entitled to is reported as a read of the inaccessible page beside it (docs/technical-notes.md, "The client itself").
- **A stack overflow ends the title** (`virtual.c`): a title that has lost threads and lives on cannot be closed by the console in time.
- **Unix libraries loaded by name** (`virtual.c`): mmdevapi asks for its audio driver that way; the same table answers. The driver itself is `wineps5_audio.c` here, compiled by `tools/wine-ps5/build-title-object.sh`, and `server/registry.c` names it as the default.
- **First frames** (`dlls/win32u/vulkan.c`): the third present tells the title, which then drops the console's splash.
- **Readable entry stubs, thread positions, profile** (`virtual.c`, `signal_x86_64.c`): `virtual_readable_entry`, `WINEPS5_THREAD_POSITIONS`, `WINEPS5_PROFILE` (docs/technical-notes.md).
- **`WINEPS5_UNIXLIBS`** (`virtual.c`, host builds only): limits a run on the PC to the Unix libraries the title has.
- **Desktop** (`dlls/win32u/winstation.c`, `driver.c`, `sysparams.c`, `server/window.c`, `winstation.c`): a thread of the process does what `explorer.exe /desktop` does in another process.
- **Vulkan and the display** (`dlls/win32u/vulkan.c`, `sysparams.c`): the RADV driver linked into the title in place of a loaded `libvulkan`; the console's output as the first window surface and as the display mode Windows sees.
- **Directories** (`file.c`): a directory is listed, and a path relative to a directory handle is opened, by the directory's path; a title has no working directory.
- **System information** (`system.c`, `virtual.c`): the direct-memory pool as physical memory, the affinity mask as the processor count.
- **TCP self-connect** (`server/sock.c`, PS5 only): a connection to its own bound endpoint reports success but never delivers bytes on this kernel. Refuse it so applications can use their fallback wake-up sockets; ordinary loopback connections continue to work. See `docs/handoffs/2026-10-05-socket-continuation.md` and `tools/win32-selfconnect-test.c`.

Calls this firmware refuses or lacks are replaced when the title object is made (`tools/wine-ps5/build-title-object.sh`, `app/runtime/wine_title.c`): `dup` goes through descriptor passing on a socket pair, relative paths through the title's own working directory, non-blocking sockets through `SO_NBIO`, name resolution through the console's resolver, `exit` through the shell. The table in docs/technical-notes.md has every one.

Build and run on the console:

```sh
tools/wine-ps5/build-console.sh                      # patched Wine objects -> title -> deploy
tools/wine-ps5/stage-console.py --all --tree work/wine-memsim-host --prefix work/console-prefix build/windows/win32-runtime-test.exe
tools/wine-ps5/run-console.py /data/wowps5/test/win32-runtime-test.exe
```

After changing anything under `vendor/wine`, run `tools/wine-ps5/refresh-patch.sh`: it rewrites the patch and checks that the patch reproduces the tree.
