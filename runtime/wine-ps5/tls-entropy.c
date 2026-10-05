/* Console entropy backend for GnuTLS. LGPL-2.1-or-later.
 * This replaces GnuTLS's device-file entropy backend in the PS5 build only.
 * Keep GnuTLS's own generator and reseeding; fail if secure entropy fails. */
#include "gnutls_int.h"
#include "rnd-common.h"
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <errno.h>

int sceKernelLoadStartModule(const char *path, size_t argc, const void *argv,
                            unsigned flags, void *options, int *result);
int sceKernelDlsym(int handle, const char *name, void **address);
int sceKernelGetModuleList(int *handles, size_t capacity, size_t *count);
int sceSysmoduleLoadModule(unsigned short id);

static int (*random_service)(uint8_t *buffer, size_t size);
static int use_kernel_random;

static int console_entropy(void *buffer, size_t size)
{
    uint8_t *out = buffer;
    while (size)
    {
        size_t chunk = size > 64 ? 64 : size;
        int result;
        if (use_kernel_random)
        {
            int mib[] = {CTL_KERN, KERN_ARND};
            size_t got = chunk;
            result = sysctl(mib, 2, out, &got, NULL, 0);
            if (!result && got != chunk) result = -1;
        }
        else result = random_service ? random_service(out, chunk) : -1;
        if (result != 0)
        {
            fprintf(stderr, "[WoWPS5 TLS] secure random failed: %#x\n", result);
            return GNUTLS_E_RANDOM_DEVICE_ERROR;
        }
        out += chunk;
        size -= chunk;
    }
    return 0;
}

get_entropy_func _rnd_get_system_entropy = console_entropy;

int _rnd_system_entropy_init(void)
{
    /* FreeBSD's kernel entropy interface, never the platform's PRNG shim. */
    int mib[] = {CTL_KERN, KERN_ARND}; uint8_t kernel_probe[32]; size_t bytes = sizeof(kernel_probe);
    int status = sysctl(mib, 2, kernel_probe, &bytes, NULL, 0);
    if (!status && bytes == sizeof(kernel_probe))
    {
        use_kernel_random = 1;
        fprintf(stderr, "[WoWPS5 TLS] kernel secure random available\n");
        return 0;
    }
    fprintf(stderr, "[WoWPS5 TLS] kernel random unavailable: status=%d errno=%d bytes=%zu\n", status, errno, bytes);
    if (!random_service)
    {
        /* This is a system SPRX, supported by the platform's module loader. */
        int loaded = sceSysmoduleLoadModule(0x00ba); /* SCE_SYSMODULE_RANDOM */
        int handles[128]; size_t count = 0;
        if (!sceKernelGetModuleList(handles, 128, &count))
            for (size_t i = 0; i < count && i < 128 && !random_service; i++)
                sceKernelDlsym(handles[i], "sceRandomGetRandomNumber", (void **)&random_service);
        if (random_service)
        {
            uint8_t probe;
            return console_entropy(&probe, sizeof(probe));
        }
        int result, module = sceKernelLoadStartModule("/system/common/lib/libSceRandom.sprx",
                                                     0, NULL, 0, NULL, &result);
        int lookup = module >= 0 ? sceKernelDlsym(module, "sceRandomGetRandomNumber", (void **)&random_service) : -1;
        fprintf(stderr, "[WoWPS5 TLS] random load=%#x modules=%zu module=%#x lookup=%#x\n", loaded, count, module, lookup);
        if (module < 0 || lookup < 0)
            return GNUTLS_E_RANDOM_DEVICE_ERROR;
    }
    uint8_t probe;
    return console_entropy(&probe, sizeof(probe));
}

void _rnd_system_entropy_deinit(void) {}
