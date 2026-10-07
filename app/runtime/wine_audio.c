/* The console's audio port for Wine's PS5 audio driver. MIT license.
 * The driver (runtime/wine-ps5/wineps5_audio.c) mixes what Windows programs
 * play into grains of 16-bit stereo frames and calls here with each one.
 * sceAudioOutOutput returns once the port has taken a grain, which is what
 * paces the driver's mixer thread.
 *
 * The port is opened the first time Windows asks for an audio device, never
 * closed (the console closes it with the title), and nothing here is waited
 * for when the title ends. WOWPS5_AUDIO=0 in the run request keeps the port
 * shut: Windows programs are then told there is no audio device, as they
 * were before this driver existed. */
#ifdef __PROSPERO__
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Called as ordinary functions only: a title's imports taken by address can
 * stay null where the call itself resolves (app/runtime/wine_devices.c). */
int sceAudioOutInit(void);
int sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t grain, uint32_t rate, uint32_t format);
int sceAudioOutOutput(int32_t port, const void *samples);

#define AUDIO_ALREADY_INITIALISED ((int)0x8026000e)
#define AUDIO_USER_SYSTEM 0xff      /* the port every user hears */
#define AUDIO_PORT_MAIN 0
#define AUDIO_S16_STEREO 1

static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;
static int audio_state;             /* 0 not tried, 1 open, -1 no port */
static int32_t audio_port = -1;
static unsigned audio_failures;

int wowps5_audio_open(unsigned rate, unsigned grain)
{
    pthread_mutex_lock(&audio_lock);
    if (!audio_state)
    {
        const char *setting = getenv("WOWPS5_AUDIO");
        audio_state = -1;
        if (setting && setting[0] == '0')
            fprintf(stderr, "[WoWPS5 audio] WOWPS5_AUDIO=0: no audio device for Windows\n");
        else
        {
            int init = sceAudioOutInit(), port = -1;
            if (init >= 0 || init == AUDIO_ALREADY_INITIALISED)
                port = sceAudioOutOpen(AUDIO_USER_SYSTEM, AUDIO_PORT_MAIN, 0, grain, rate, AUDIO_S16_STEREO);
            if (port >= 0) { audio_port = port; audio_state = 1; }
            fprintf(stderr, "[WoWPS5 audio] sceAudioOutInit=%#x sceAudioOutOpen=%#x: %s (%u Hz, 16-bit stereo, grains of %u frames)\n",
                (unsigned)init, (unsigned)port, port >= 0 ? "port open" : "NO PORT, no audio device for Windows", rate, grain);
        }
        fflush(stderr);
    }
    int open = audio_state > 0;
    pthread_mutex_unlock(&audio_lock);
    return open;
}

/* One grain of the size the port was opened with. Only the driver's mixer thread calls this. */
int wowps5_audio_output(const int16_t *frames)
{
    if (audio_state <= 0) return -1;
    int result = sceAudioOutOutput(audio_port, frames);
    if (result >= 0) return 0;
    /* said once, then every 4096th time: the driver keeps time by the clock meanwhile */
    if (!(audio_failures++ & 4095))
    {
        fprintf(stderr, "[WoWPS5 audio] sceAudioOutOutput=%#x (failure %u)\n", (unsigned)result, audio_failures);
        fflush(stderr);
    }
    return -1;
}
#endif
