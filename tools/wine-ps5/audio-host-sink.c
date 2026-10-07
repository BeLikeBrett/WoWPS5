/* A stand-in for the console's audio port, for testing Wine's PS5 audio driver
 * on the PC (tools/wine-ps5/test-audio-host.sh). MIT license.
 *
 * It has the title's two entries (app/runtime/wine_audio.c). Grains are
 * appended to the file WOWPS5_AUDIO_CAPTURE names, as raw 16-bit stereo.
 * WOWPS5_AUDIO_HOST_PORT chooses how the stand-in behaves:
 *   block    (default) returns when the grain before has "played", as the
 *            console's port does: one grain of real time per call
 *   noblock  returns at once: a port that does not pace its caller
 *   refuse   takes nothing and reports failure, at once
 * WOWPS5_AUDIO=0 refuses to open, as on the console.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static FILE *capture;
static unsigned port_rate, port_grain;
static enum { BLOCK, NOBLOCK, REFUSE } mode;
static uint64_t next_free;      /* when the port can take another grain, ns */

static uint64_t now_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

__attribute__((visibility("hidden"))) int wowps5_audio_open(unsigned rate, unsigned grain)
{
    const char *setting = getenv("WOWPS5_AUDIO"), *path = getenv("WOWPS5_AUDIO_CAPTURE"), *port = getenv("WOWPS5_AUDIO_HOST_PORT");
    if (setting && setting[0] == '0') { fprintf(stderr, "[host audio] WOWPS5_AUDIO=0: no port\n"); return 0; }
    if (capture) return 1;
    if (!path || !(capture = fopen(path, "wb"))) { fprintf(stderr, "[host audio] no capture file\n"); return 0; }
    mode = port && !strcmp(port, "noblock") ? NOBLOCK : port && !strcmp(port, "refuse") ? REFUSE : BLOCK;
    port_rate = rate; port_grain = grain;
    fprintf(stderr, "[host audio] port open: %u Hz, grains of %u frames, mode %d, capture %s\n", rate, grain, mode, path);
    return 1;
}

__attribute__((visibility("hidden"))) int wowps5_audio_output(const int16_t *frames)
{
    if (!capture || mode == REFUSE) return -1;
    fwrite(frames, 2 * sizeof(int16_t), port_grain, capture);
    fflush(capture);
    if (mode == BLOCK)
    {
        /* one grain is queued behind the one playing; the call returns when that queue has room */
        const uint64_t grain_ns = (uint64_t)port_grain * 1000000000u / port_rate;
        uint64_t now = now_ns();
        if (next_free < now) next_free = now;
        else
        {
            struct timespec wait = { 0, (long)(next_free - now) };
            nanosleep(&wait, NULL);
        }
        next_free += grain_ns;
    }
    return 0;
}
