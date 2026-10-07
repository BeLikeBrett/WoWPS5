/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Wine audio driver for the PS5 title, Unix side ("wineps5.so").
 *
 * mmdevapi loads an audio driver by name, as a Unix library with no Windows
 * side: HKCU\Software\Wine\Drivers\Audio = "ps5" makes it ask for
 * wineps5.drv, which ntdll looks up as wineps5.so. A title cannot dlopen, so
 * this file is compiled into the title and listed in its table of Unix
 * libraries (tools/wine-ps5/build-title-object.sh).
 *
 * There is one render endpoint, stereo at 48 kHz, and no capture. Every
 * stream keeps mmdevapi's ring of frames in the format its client chose.
 * One Unix thread mixes all started streams into a grain of 256 16-bit
 * stereo frames (any PCM or float format, 1 to 8 channels, any rate: winmm,
 * dsound and xaudio2 open theirs with AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM)
 * and hands the grain to the title's audio port (app/runtime/wine_audio.c).
 * The port returns once it has taken the grain, so the port is the clock:
 * frames leave the rings, positions advance and stream events are set at the
 * rate the console plays. Should the port refuse grains or stop blocking,
 * the thread keeps time against the monotonic clock instead: it never runs
 * ahead of real time.
 *
 * The stream, buffer and position code follows Wine's OSS driver
 * (dlls/wineoss.drv/oss.c, Copyright 2011 Andrew Eikum for CodeWeavers,
 * 2022 Huw Davies). One mixer thread clocked by the console's port, and the
 * conversion of each stream to the port's format inside the driver, is the
 * design of prospero-win's PS5 driver (wine/wineps5/unix.c and
 * src/pw_audio_mix.c, LGPL-2.1-or-later); the code here is written for this
 * title.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "audioclient.h"
#include "mmdeviceapi.h"
#include "mmddk.h"

#include "wine/debug.h"
#include "wine/list.h"
#include "wine/unixlib.h"

#include "unixlib.h"    /* dlls/mmdevapi/unixlib.h: the driver interface */

WINE_DEFAULT_DEBUG_CHANNEL(ps5audio);

/* The title's audio port. open: 1 when grains of that many 16-bit stereo frames
 * can be played at that rate. output: one grain; returns once the port has
 * taken it, below 0 when it was not played. */
extern int wowps5_audio_open( unsigned int rate, unsigned int grain );
extern int wowps5_audio_output( const short *frames );

#define PS5_RATE      48000
#define PS5_GRAIN     256
#define PS5_CHANNELS  8                     /* the most a stream may have */
#define PHASE_ONE     ((UINT64)1 << 32)

enum sample_type { SAMPLE_U8, SAMPLE_S16, SAMPLE_S24, SAMPLE_S32, SAMPLE_FLOAT };

struct ps5_stream
{
    struct list entry;
    WAVEFORMATEX *fmt;
    AUDCLNT_SHAREMODE share;
    UINT flags;
    HANDLE event;
    BOOL playing;

    UINT64 written_frames, last_pos_frames;
    UINT32 period_frames, bufsize_frames, held_frames, tmp_buffer_frames;
    UINT32 lcl_offs_frames;                 /* offs into local_buffer where valid data starts */
    UINT32 period_out_frames, since_event;  /* at the port's rate */
    REFERENCE_TIME period;

    BYTE *local_buffer, *tmp_buffer;
    INT32 getbuf_last;                      /* <0 when using tmp_buffer */

    /* the stream as the mixer reads it */
    enum sample_type type;
    UINT32 channels, sample_bytes;
    float weight[PS5_CHANNELS][2];          /* each channel's share of left and right */
    float left[PS5_CHANNELS], right[PS5_CHANNELS];  /* the same, times the channel's volume */
    UINT64 step, phase;                     /* stream frames per port frame, and position: 32.32 */
    float previous[2], current[2];          /* the two stream frames the position lies between */
    UINT32 underruns;                       /* grains the ring could not fill */
};

/* The three identifiers the driver compares, as its own constants: initguid.h
 * would define every GUID of the headers above as a global of the title. */
static const GUID subtype_pcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID subtype_float = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const PROPERTYKEY physical_speakers_key =    /* PKEY_AudioEndpoint_PhysicalSpeakers */
    {{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 3};

static const REFERENCE_TIME def_period = 100000;
static const REFERENCE_TIME min_period = 50000;
static const char device_name[] = "ps5";
static const WCHAR endpoint_name[] = {'P','S','5',0};

/* One lock for every stream: the mixer reads them all for each grain. It is
 * never held across a call that waits (the port, a sleep).
 *
 * A thread must not die holding it. When a Windows program exits, Wine ends
 * its other threads where they stand, with a signal, and whatever the program
 * calls after that (a library's last Release) would wait for the lock for
 * ever: a title that does not end. So the lock is taken with those signals
 * held back, as ntdll takes its own, and no call to the server is made under
 * it, since a server call ends a thread the server has already killed. */
static pthread_mutex_t ps5_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ps5_started = PTHREAD_COND_INITIALIZER;
static sigset_t ps5_signals;                /* what Wine interrupts or ends a thread with */
static struct list ps5_streams = LIST_INIT( ps5_streams );
static LONG ps5_mixer_started;
static int ps5_port = -1;                   /* -1 not asked yet, 0 no port, 1 open */
static unsigned int ps5_stream_count;       /* streams ever created, for the log */

static void ps5_enter( sigset_t *saved )
{
    pthread_sigmask( SIG_BLOCK, &ps5_signals, saved );
    pthread_mutex_lock( &ps5_lock );
}

static void ps5_leave( const sigset_t *saved )
{
    pthread_mutex_unlock( &ps5_lock );
    pthread_sigmask( SIG_SETMASK, saved, NULL );
}

static NTSTATUS ps5_not_implemented( void *args )
{
    return STATUS_SUCCESS;
}

/* copied from kernelbase */
static int muldiv( int a, int b, int c )
{
    LONGLONG ret;

    if (!c) return -1;

    /* We want to deal with a positive divisor to simplify the logic. */
    if (c < 0)
    {
        a = -a;
        c = -c;
    }

    /* If the result is positive, we "add" to round. else, we subtract to round. */
    if ((a < 0 && b < 0) || (a >= 0 && b >= 0))
        ret = (((LONGLONG)a * b) + (c / 2)) / c;
    else
        ret = (((LONGLONG)a * b) - (c / 2)) / c;

    if (ret > 2147483647 || ret < -2147483647) return -1;
    return ret;
}

static NTSTATUS leave_result( const sigset_t *saved, HRESULT *result, HRESULT value )
{
    *result = value;
    ps5_leave( saved );
    return STATUS_SUCCESS;
}

static struct ps5_stream *handle_get_stream( stream_handle h )
{
    return (struct ps5_stream *)(UINT_PTR)h;
}

static BOOL is_device( const char *device, EDataFlow flow )
{
    return flow == eRender && device && !strcmp( device, device_name );
}

static BOOL port_available(void)
{
    sigset_t saved;
    BOOL ret;

    ps5_enter( &saved );
    if (ps5_port < 0) ps5_port = wowps5_audio_open( PS5_RATE, PS5_GRAIN ) > 0;
    ret = ps5_port > 0;
    ps5_leave( &saved );
    return ret;
}

static NTSTATUS ps5_process_attach( void *args )
{
    /* the signals ntdll holds back around its own locks (server_block_set) */
    sigemptyset( &ps5_signals );
    sigaddset( &ps5_signals, SIGALRM );
    sigaddset( &ps5_signals, SIGIO );
    sigaddset( &ps5_signals, SIGINT );
    sigaddset( &ps5_signals, SIGHUP );
    sigaddset( &ps5_signals, SIGQUIT );
    sigaddset( &ps5_signals, SIGUSR1 );
    sigaddset( &ps5_signals, SIGUSR2 );
    sigaddset( &ps5_signals, SIGCHLD );
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_test_connect( void *args )
{
    struct test_connect_params *params = args;

    /* without a port there is no device: mmdevapi then reports no driver, as before */
    params->priority = port_available() ? Priority_Preferred : Priority_Unavailable;
    TRACE( "priority %d\n", params->priority );
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_get_endpoint_ids( void *args )
{
    struct get_endpoint_ids_params *params = args;
    unsigned int num = params->flow == eRender && port_available() ? 1 : 0;
    unsigned int offset, needed;

    offset = needed = num * sizeof(*params->endpoints);
    if (num) needed += sizeof(endpoint_name) + ((sizeof(device_name) + 1) & ~1);

    params->num = num;
    params->default_idx = 0;
    if (needed > params->size)
    {
        params->size = needed;
        params->result = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        return STATUS_SUCCESS;
    }
    if (num)
    {
        params->endpoints[0].name = offset;
        memcpy( (char *)params->endpoints + offset, endpoint_name, sizeof(endpoint_name) );
        offset += sizeof(endpoint_name);
        params->endpoints[0].device = offset;
        memcpy( (char *)params->endpoints + offset, device_name, sizeof(device_name) );
    }
    params->result = S_OK;
    return STATUS_SUCCESS;
}

/* The layout of a format that names fewer speakers than it has channels
 * (as get_channel_mask in Wine's OSS driver). */
static DWORD usual_channel_mask( unsigned int channels )
{
    switch (channels)
    {
    case 1: return KSAUDIO_SPEAKER_MONO;
    case 2: return KSAUDIO_SPEAKER_STEREO;
    case 3: return KSAUDIO_SPEAKER_STEREO | SPEAKER_LOW_FREQUENCY;
    case 4: return KSAUDIO_SPEAKER_QUAD;
    case 5: return KSAUDIO_SPEAKER_QUAD | SPEAKER_LOW_FREQUENCY;
    case 6: return KSAUDIO_SPEAKER_5POINT1;
    case 7: return KSAUDIO_SPEAKER_5POINT1 | SPEAKER_BACK_CENTER;
    case 8: return KSAUDIO_SPEAKER_7POINT1_SURROUND;
    }
    return 0;
}

/* How the mixer reads a format; AUDCLNT_E_UNSUPPORTED_FORMAT for what it cannot play.
 * mmdevapi has checked the format's consistency (validate_fmt) before the driver sees it. */
static HRESULT read_format( const WAVEFORMATEX *fmt, enum sample_type *type, DWORD *mask )
{
    const WAVEFORMATEXTENSIBLE *fmtex = (const WAVEFORMATEXTENSIBLE *)fmt;
    BOOL is_float;

    *mask = 0;
    if (fmt->wFormatTag == WAVE_FORMAT_PCM) is_float = FALSE;
    else if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) is_float = TRUE;
    else if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             fmt->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
    {
        if (IsEqualGUID( &fmtex->SubFormat, &subtype_pcm )) is_float = FALSE;
        else if (IsEqualGUID( &fmtex->SubFormat, &subtype_float )) is_float = TRUE;
        else return AUDCLNT_E_UNSUPPORTED_FORMAT;
        *mask = fmtex->dwChannelMask;
    }
    else return AUDCLNT_E_UNSUPPORTED_FORMAT;

    if (!fmt->nChannels || fmt->nChannels > PS5_CHANNELS) return AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (fmt->nSamplesPerSec < 1000 || fmt->nSamplesPerSec > 384000) return AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (fmt->nBlockAlign != fmt->nChannels * fmt->wBitsPerSample / 8) return AUDCLNT_E_UNSUPPORTED_FORMAT;

    if (is_float)
    {
        if (fmt->wBitsPerSample != 32) return AUDCLNT_E_UNSUPPORTED_FORMAT;
        *type = SAMPLE_FLOAT;
    }
    else switch (fmt->wBitsPerSample)
    {
    case 8:  *type = SAMPLE_U8; break;
    case 16: *type = SAMPLE_S16; break;
    case 24: *type = SAMPLE_S24; break;
    case 32: *type = SAMPLE_S32; break;     /* 24 valid bits sit high in the container */
    default: return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    return S_OK;
}

/* Each channel's share of the left and right outputs, by its speaker. */
static void set_fold_weights( struct ps5_stream *stream, DWORD mask )
{
    static const DWORD left_side = SPEAKER_BACK_LEFT | SPEAKER_FRONT_LEFT_OF_CENTER | SPEAKER_SIDE_LEFT |
                                   SPEAKER_TOP_FRONT_LEFT | SPEAKER_TOP_BACK_LEFT;
    static const DWORD right_side = SPEAKER_BACK_RIGHT | SPEAKER_FRONT_RIGHT_OF_CENTER | SPEAKER_SIDE_RIGHT |
                                    SPEAKER_TOP_FRONT_RIGHT | SPEAKER_TOP_BACK_RIGHT;
    static const float half_power = 0.70710678f;
    unsigned int i, bits = 0;
    DWORD rest;

    for (rest = mask; rest; rest &= rest - 1) bits++;
    if (bits < stream->channels) mask = usual_channel_mask( stream->channels );

    for (i = 0; i < stream->channels; i++)
    {
        DWORD speaker = mask & -mask;
        float left = 0.0f, right = 0.0f;

        mask &= mask - 1;
        if (stream->channels == 1) left = right = 1.0f;
        else if (speaker == SPEAKER_FRONT_LEFT) left = 1.0f;
        else if (speaker == SPEAKER_FRONT_RIGHT) right = 1.0f;
        else if (speaker & left_side) left = half_power;
        else if (speaker & right_side) right = half_power;
        else if (speaker != SPEAKER_LOW_FREQUENCY) left = right = half_power;  /* a centre speaker */
        /* the low-frequency channel is left out, as in the usual stereo fold-down */
        stream->weight[i][0] = stream->left[i] = left;
        stream->weight[i][1] = stream->right[i] = right;
    }
}

static WAVEFORMATEXTENSIBLE *clone_format( const WAVEFORMATEX *fmt )
{
    WAVEFORMATEXTENSIBLE *ret;
    size_t size;

    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        size = sizeof(WAVEFORMATEXTENSIBLE);
    else
        size = sizeof(WAVEFORMATEX);

    ret = malloc( size );
    if (!ret) return NULL;

    memcpy( ret, fmt, size );
    ret->Format.cbSize = size - sizeof(WAVEFORMATEX);
    return ret;
}

static void free_stream( struct ps5_stream *stream )
{
    SIZE_T size;

    if (stream->local_buffer)
    {
        size = 0;
        NtFreeVirtualMemory( NtCurrentProcess(), (void **)&stream->local_buffer, &size, MEM_RELEASE );
    }
    if (stream->tmp_buffer)
    {
        size = 0;
        NtFreeVirtualMemory( NtCurrentProcess(), (void **)&stream->tmp_buffer, &size, MEM_RELEASE );
    }
    free( stream->fmt );
    free( stream );
}

static void reset_resampler( struct ps5_stream *stream )
{
    stream->phase = PHASE_ONE;              /* the first port frame takes a stream frame */
    stream->previous[0] = stream->previous[1] = stream->current[0] = stream->current[1] = 0.0f;
}

static NTSTATUS ps5_create_stream( void *args )
{
    struct create_stream_params *params = args;
    WAVEFORMATEXTENSIBLE *fmtex;
    struct ps5_stream *stream;
    enum sample_type type;
    unsigned int number;
    sigset_t saved;
    DWORD mask;
    SIZE_T size;

    if (!is_device( params->device, params->flow ))
    {
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }
    if (FAILED(params->result = read_format( params->fmt, &type, &mask )))
        return STATUS_SUCCESS;

    stream = calloc( 1, sizeof(*stream) );
    if (!stream)
    {
        params->result = E_OUTOFMEMORY;
        return STATUS_SUCCESS;
    }

    fmtex = clone_format( params->fmt );
    if (!fmtex)
    {
        params->result = E_OUTOFMEMORY;
        goto exit;
    }
    stream->fmt = &fmtex->Format;

    stream->period = params->period;
    stream->period_frames = muldiv( params->fmt->nSamplesPerSec, params->period, 10000000 );
    stream->period_out_frames = muldiv( PS5_RATE, params->period, 10000000 );
    if ((int)stream->period_frames <= 0 || (int)stream->period_out_frames <= 0)
    {
        params->result = E_INVALIDARG;
        goto exit;
    }

    stream->bufsize_frames = muldiv( params->duration, params->fmt->nSamplesPerSec, 10000000 );
    if ((int)stream->bufsize_frames <= 0)
    {
        params->result = AUDCLNT_E_BUFFER_SIZE_ERROR;
        goto exit;
    }
    if (params->share == AUDCLNT_SHAREMODE_EXCLUSIVE)
        stream->bufsize_frames -= stream->bufsize_frames % stream->period_frames;
    if (!stream->bufsize_frames)
    {
        params->result = AUDCLNT_E_BUFFER_SIZE_ERROR;
        goto exit;
    }
    size = (SIZE_T)stream->bufsize_frames * params->fmt->nBlockAlign;
    if (NtAllocateVirtualMemory( NtCurrentProcess(), (void **)&stream->local_buffer, 0,
                                 &size, MEM_COMMIT, PAGE_READWRITE ))
    {
        params->result = E_OUTOFMEMORY;
        goto exit;
    }

    stream->share = params->share;
    stream->flags = params->flags;
    stream->type = type;
    stream->channels = params->fmt->nChannels;
    stream->sample_bytes = params->fmt->wBitsPerSample / 8;
    stream->step = ((UINT64)params->fmt->nSamplesPerSec << 32) / PS5_RATE;
    set_fold_weights( stream, mask );
    reset_resampler( stream );

    ps5_enter( &saved );
    list_add_tail( &ps5_streams, &stream->entry );
    number = ++ps5_stream_count;
    ps5_leave( &saved );

    /* the first streams are worth a line each without any debug channel: what the program asked for */
    if (number <= 8)
        MESSAGE( "wineps5: stream %u: %u Hz, %u channels, %u-bit %s, %s%s, buffer %u frames, period %u frames\n",
                 number, (unsigned int)params->fmt->nSamplesPerSec, params->fmt->nChannels,
                 params->fmt->wBitsPerSample, type == SAMPLE_FLOAT ? "float" : "PCM",
                 params->share == AUDCLNT_SHAREMODE_EXCLUSIVE ? "exclusive" : "shared",
                 params->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK ? ", event-driven" : "",
                 (unsigned int)stream->bufsize_frames, (unsigned int)stream->period_frames );
    else
        TRACE( "stream %u: %u Hz, %u channels, %u bits\n", number, (unsigned int)params->fmt->nSamplesPerSec,
               params->fmt->nChannels, params->fmt->wBitsPerSample );

exit:
    if (FAILED(params->result))
        free_stream( stream );
    else
    {
        *params->channel_count = params->fmt->nChannels;
        *params->stream = (stream_handle)(UINT_PTR)stream;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_release_stream( void *args )
{
    struct release_stream_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    sigset_t saved;

    /* the mixer only touches streams that are in the list, under the lock */
    ps5_enter( &saved );
    list_remove( &stream->entry );
    ps5_leave( &saved );

    free_stream( stream );
    params->result = S_OK;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- the mixer */

static inline float read_sample( const BYTE *at, enum sample_type type )
{
    float value;

    switch (type)
    {
    case SAMPLE_U8:
        return ((int)at[0] - 128) * (1.0f / 128);
    case SAMPLE_S16:
        return (short)(at[0] | at[1] << 8) * (1.0f / 32768);
    case SAMPLE_S24:
        return (float)((int)((UINT)at[0] << 8 | (UINT)at[1] << 16 | (UINT)at[2] << 24) >> 8) * (1.0f / 8388608);
    case SAMPLE_S32:
        return (float)(int)((UINT)at[0] | (UINT)at[1] << 8 | (UINT)at[2] << 16 | (UINT)at[3] << 24)
               * (1.0f / 2147483648.0f);
    default:
        memcpy( &value, at, sizeof(value) );
        /* streams may exceed full scale and the sum is clamped, but a NaN or an infinity would poison it */
        if (!(value == value)) return 0.0f;
        return value > 8.0f ? 8.0f : value < -8.0f ? -8.0f : value;
    }
}

/* The oldest held frame, folded to stereo, becomes the resampler's current one. */
static void take_frame( struct ps5_stream *stream )
{
    const BYTE *at = stream->local_buffer + (SIZE_T)stream->lcl_offs_frames * stream->fmt->nBlockAlign;
    float left = 0.0f, right = 0.0f;
    unsigned int i;

    for (i = 0; i < stream->channels; i++, at += stream->sample_bytes)
    {
        float value = read_sample( at, stream->type );
        left += value * stream->left[i];
        right += value * stream->right[i];
    }
    stream->previous[0] = stream->current[0];
    stream->previous[1] = stream->current[1];
    stream->current[0] = left;
    stream->current[1] = right;
    if (++stream->lcl_offs_frames == stream->bufsize_frames) stream->lcl_offs_frames = 0;
    stream->held_frames--;
}

/* Add a grain of the stream, resampled linearly to the port's rate, to the sum.
 * A ring that runs out adds silence for the rest and resumes where it stopped. */
static void mix_stream( struct ps5_stream *stream, float *sum )
{
    unsigned int i;

    for (i = 0; i < PS5_GRAIN; i++)
    {
        float t;

        while (stream->phase >= PHASE_ONE)
        {
            if (!stream->held_frames)
            {
                stream->underruns++;
                return;
            }
            take_frame( stream );
            stream->phase -= PHASE_ONE;
        }
        t = (float)(UINT32)stream->phase * (1.0f / 4294967296.0f);
        sum[2 * i] += stream->previous[0] + (stream->current[0] - stream->previous[0]) * t;
        sum[2 * i + 1] += stream->previous[1] + (stream->current[1] - stream->previous[1]) * t;
        stream->phase += stream->step;
    }
}

static UINT64 monotonic_ns(void)
{
    struct timespec now;

    clock_gettime( CLOCK_MONOTONIC, &now );
    return (UINT64)now.tv_sec * 1000000000 + now.tv_nsec;
}

static void sleep_ns( UINT64 ns )
{
    struct timespec delay;

    if (ns > 100000000) ns = 100000000;     /* never long: the stream list may have changed */
    delay.tv_sec = 0;
    delay.tv_nsec = (long)ns;
    nanosleep( &delay, NULL );
}

/* The mixer: every started stream into one grain, then the grain to the port. */
static void mixer_loop( void *args )
{
    static float sum[2 * PS5_GRAIN];
    static short grain[2 * PS5_GRAIN];
    const BOOL diagnostics = getenv( "WOWPS5_AUDIO_DIAGNOSTICS" ) != NULL;
    UINT64 epoch = 0, frames = 0;           /* since the mixer last began to play */
    UINT64 started = 0, grains = 0, audible = 0, refused = 0, held_back = 0, next_report = 0;
    HANDLE events[32];
    struct ps5_stream *stream;
    unsigned int i, playing, starved, nb_events, peak;
    UINT64 now, due;
    sigset_t saved;
    int status;

    ps5_enter( &saved );
    for (;;)
    {
        playing = starved = nb_events = 0;
        LIST_FOR_EACH_ENTRY( stream, &ps5_streams, struct ps5_stream, entry ) playing += stream->playing;
        if (!playing)
        {
            /* Waits with the signals held back: an idle mixer is not ended with the
             * program's other threads, it ends with the title. It holds nothing then. */
            pthread_cond_wait( &ps5_started, &ps5_lock );
            epoch = 0;
            continue;
        }

        memset( sum, 0, sizeof(sum) );
        LIST_FOR_EACH_ENTRY( stream, &ps5_streams, struct ps5_stream, entry )
        {
            UINT32 underruns = stream->underruns;

            if (!stream->playing) continue;
            mix_stream( stream, sum );
            starved += stream->underruns != underruns;
            /* the frames have left the ring: room for the client's next period */
            stream->since_event += PS5_GRAIN;
            if (stream->since_event < stream->period_out_frames) continue;
            if (stream->event)
            {
                if (nb_events == ARRAY_SIZE(events)) continue;  /* with the next grain */
                events[nb_events++] = stream->event;
            }
            stream->since_event %= stream->period_out_frames;
        }
        ps5_leave( &saved );

        /* Server calls, so not under the lock. A stream released since then has closed
         * its event; setting a closed handle fails and is nothing. */
        for (i = 0; i < nb_events; i++) NtSetEvent( events[i], NULL );

        peak = 0;
        for (i = 0; i < 2 * PS5_GRAIN; i++)
        {
            float value = sum[i] > 1.0f ? 1.0f : sum[i] < -1.0f ? -1.0f : sum[i];
            int sample = (int)(value * 32767.0f + (value >= 0.0f ? 0.5f : -0.5f));
            unsigned int size = sample < 0 ? -sample : sample;

            grain[i] = sample;
            if (size > peak) peak = size;
        }

        now = monotonic_ns();
        if (!epoch)
        {
            epoch = now;
            frames = 0;
        }
        if (!started) started = now;
        status = wowps5_audio_output( grain );
        frames += PS5_GRAIN;
        grains++;
        if (status < 0) refused++;
        if (grains == 1)
            MESSAGE( "wineps5: the mixer runs: first grain %s\n", status < 0 ? "REFUSED by the port" : "taken by the port" );
        if (peak && !audible++)
            MESSAGE( "wineps5: first grain with sound in it (peak %u of 32767) is grain %u\n",
                     peak, (unsigned int)grains );

        /* The port is the clock. Against the monotonic clock: a refused grain's
         * time is slept, a port that does not block is not allowed to run the
         * rings dry, and a stall is not caught up afterwards. */
        now = monotonic_ns();
        due = epoch + frames * 1000000000 / PS5_RATE;
        if (status < 0)
        {
            if (due > now) sleep_ns( due - now );
        }
        else if (due > now + 60000000)
        {
            held_back++;
            sleep_ns( due - now - 40000000 );
        }
        else if (now > due + 250000000)
        {
            epoch = now;
            frames = 0;
        }

        /* one line after about two seconds of playing, then only when asked for */
        if (grains == 375 || (diagnostics && now >= next_report))
        {
            MESSAGE( "wineps5: %u grains in %u ms (%u ms of sound), %u with sound, %u refused, %u held back; "
                     "now %u streams playing, %u with an empty ring\n",
                     (unsigned int)grains, (unsigned int)((now - started) / 1000000),
                     (unsigned int)(grains * PS5_GRAIN * 1000 / PS5_RATE), (unsigned int)audible,
                     (unsigned int)refused, (unsigned int)held_back, playing, starved );
            next_report = now + (UINT64)10 * 1000000000;
        }

        ps5_enter( &saved );
    }
}

static NTSTATUS ps5_start( void *args )
{
    struct start_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    static const WCHAR name[] = {'w','i','n','e','p','s','5','_','m','i','x','e','r',0};
    sigset_t saved;
    HANDLE thread;

    /* the one mixer thread, made by whoever starts the first stream; a server call, so before the lock */
    if (!InterlockedCompareExchange( &ps5_mixer_started, 1, 0 ))
    {
        if (create_unix_thread( &thread, name, mixer_loop, NULL ))
        {
            InterlockedExchange( &ps5_mixer_started, 0 );
            ERR( "no mixer thread\n" );
            params->result = E_OUTOFMEMORY;
            return STATUS_SUCCESS;
        }
        NtClose( thread );
    }

    ps5_enter( &saved );

    if ((stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !stream->event)
        return leave_result( &saved, &params->result, AUDCLNT_E_EVENTHANDLE_NOT_SET );

    if (stream->playing)
        return leave_result( &saved, &params->result, AUDCLNT_E_NOT_STOPPED );

    stream->playing = TRUE;
    stream->since_event = 0;
    pthread_cond_signal( &ps5_started );

    return leave_result( &saved, &params->result, S_OK );
}

static NTSTATUS ps5_stop( void *args )
{
    struct stop_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    sigset_t saved;

    ps5_enter( &saved );

    if (!stream->playing)
        return leave_result( &saved, &params->result, S_FALSE );

    stream->playing = FALSE;

    return leave_result( &saved, &params->result, S_OK );
}

static NTSTATUS ps5_reset( void *args )
{
    struct reset_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    sigset_t saved;

    ps5_enter( &saved );

    if (stream->playing)
        return leave_result( &saved, &params->result, AUDCLNT_E_NOT_STOPPED );

    if (stream->getbuf_last)
        return leave_result( &saved, &params->result, AUDCLNT_E_BUFFER_OPERATION_PENDING );

    stream->written_frames = 0;
    stream->last_pos_frames = 0;
    stream->held_frames = 0;
    stream->lcl_offs_frames = 0;
    stream->since_event = 0;
    reset_resampler( stream );

    return leave_result( &saved, &params->result, S_OK );
}

static void silence_buffer( struct ps5_stream *stream, BYTE *buffer, UINT32 frames )
{
    memset( buffer, stream->type == SAMPLE_U8 ? 128 : 0, (SIZE_T)frames * stream->fmt->nBlockAlign );
}

static NTSTATUS ps5_get_render_buffer( void *args )
{
    struct get_render_buffer_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    UINT32 write_pos, frames = params->frames;
    BYTE **data = params->data;
    SIZE_T size;
    sigset_t saved;

    ps5_enter( &saved );

    if (stream->getbuf_last)
        return leave_result( &saved, &params->result, AUDCLNT_E_OUT_OF_ORDER );

    if (!frames)
        return leave_result( &saved, &params->result, S_OK );

    if (stream->held_frames + frames > stream->bufsize_frames)
        return leave_result( &saved, &params->result, AUDCLNT_E_BUFFER_TOO_LARGE );

    /* where the client writes does not move while the mixer takes frames from the other end */
    write_pos = (stream->lcl_offs_frames + stream->held_frames) % stream->bufsize_frames;
    if (write_pos + frames > stream->bufsize_frames)
    {
        if (stream->tmp_buffer_frames < frames)
        {
            if (stream->tmp_buffer)
            {
                size = 0;
                NtFreeVirtualMemory( NtCurrentProcess(), (void **)&stream->tmp_buffer, &size, MEM_RELEASE );
                stream->tmp_buffer = NULL;
            }
            size = (SIZE_T)frames * stream->fmt->nBlockAlign;
            if (NtAllocateVirtualMemory( NtCurrentProcess(), (void **)&stream->tmp_buffer, 0,
                                         &size, MEM_COMMIT, PAGE_READWRITE ))
            {
                stream->tmp_buffer_frames = 0;
                return leave_result( &saved, &params->result, E_OUTOFMEMORY );
            }
            stream->tmp_buffer_frames = frames;
        }
        *data = stream->tmp_buffer;
        stream->getbuf_last = -frames;
    }
    else
    {
        *data = stream->local_buffer + (SIZE_T)write_pos * stream->fmt->nBlockAlign;
        stream->getbuf_last = frames;
    }

    silence_buffer( stream, *data, frames );

    return leave_result( &saved, &params->result, S_OK );
}

static void wrap_buffer( struct ps5_stream *stream, BYTE *buffer, UINT32 written_frames )
{
    UINT32 write_offs_frames = (stream->lcl_offs_frames + stream->held_frames) % stream->bufsize_frames;
    SIZE_T write_offs_bytes = (SIZE_T)write_offs_frames * stream->fmt->nBlockAlign;
    UINT32 chunk_frames = stream->bufsize_frames - write_offs_frames;
    SIZE_T chunk_bytes = (SIZE_T)chunk_frames * stream->fmt->nBlockAlign;
    SIZE_T written_bytes = (SIZE_T)written_frames * stream->fmt->nBlockAlign;

    if (written_bytes <= chunk_bytes)
        memcpy( stream->local_buffer + write_offs_bytes, buffer, written_bytes );
    else
    {
        memcpy( stream->local_buffer + write_offs_bytes, buffer, chunk_bytes );
        memcpy( stream->local_buffer, buffer + chunk_bytes, written_bytes - chunk_bytes );
    }
}

static NTSTATUS ps5_release_render_buffer( void *args )
{
    struct release_render_buffer_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    UINT32 written_frames = params->written_frames;
    BYTE *buffer;
    sigset_t saved;

    ps5_enter( &saved );

    if (!written_frames)
    {
        stream->getbuf_last = 0;
        return leave_result( &saved, &params->result, S_OK );
    }

    if (!stream->getbuf_last)
        return leave_result( &saved, &params->result, AUDCLNT_E_OUT_OF_ORDER );

    if (written_frames > (stream->getbuf_last >= 0 ? stream->getbuf_last : -stream->getbuf_last))
        return leave_result( &saved, &params->result, AUDCLNT_E_INVALID_SIZE );

    if (stream->getbuf_last >= 0)
        buffer = stream->local_buffer + (SIZE_T)stream->fmt->nBlockAlign *
                 ((stream->lcl_offs_frames + stream->held_frames) % stream->bufsize_frames);
    else
        buffer = stream->tmp_buffer;

    if (params->flags & AUDCLNT_BUFFERFLAGS_SILENT)
        silence_buffer( stream, buffer, written_frames );

    if (stream->getbuf_last < 0)
        wrap_buffer( stream, buffer, written_frames );

    stream->held_frames += written_frames;
    stream->written_frames += written_frames;
    stream->getbuf_last = 0;

    return leave_result( &saved, &params->result, S_OK );
}

/* There is no capture endpoint, so no stream these could be called for. */
static NTSTATUS ps5_get_capture_buffer( void *args )
{
    struct get_capture_buffer_params *params = args;

    params->result = AUDCLNT_E_WRONG_ENDPOINT_TYPE;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_release_capture_buffer( void *args )
{
    struct release_capture_buffer_params *params = args;

    params->result = AUDCLNT_E_WRONG_ENDPOINT_TYPE;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_is_format_supported( void *args )
{
    struct is_format_supported_params *params = args;
    enum sample_type type;
    DWORD mask;

    if (!is_device( params->device, params->flow ))
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
    else
        params->result = read_format( params->fmt_in, &type, &mask );
    return STATUS_SUCCESS;
}

/* What Windows calls the mix format: float stereo at the port's rate. */
static NTSTATUS ps5_get_mix_format( void *args )
{
    struct get_mix_format_params *params = args;
    WAVEFORMATEXTENSIBLE *fmt = params->fmt;

    if (!is_device( params->device, params->flow ))
    {
        params->result = params->flow == eRender ? AUDCLNT_E_DEVICE_INVALIDATED : E_UNEXPECTED;
        return STATUS_SUCCESS;
    }

    fmt->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt->Format.nChannels = 2;
    fmt->Format.nSamplesPerSec = PS5_RATE;
    fmt->Format.wBitsPerSample = 32;
    fmt->Format.nBlockAlign = fmt->Format.nChannels * fmt->Format.wBitsPerSample / 8;
    fmt->Format.nAvgBytesPerSec = fmt->Format.nSamplesPerSec * fmt->Format.nBlockAlign;
    fmt->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    fmt->Samples.wValidBitsPerSample = fmt->Format.wBitsPerSample;
    fmt->dwChannelMask = KSAUDIO_SPEAKER_STEREO;
    fmt->SubFormat = subtype_float;

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_get_device_period( void *args )
{
    struct get_device_period_params *params = args;

    if (params->def_period) *params->def_period = def_period;
    if (params->min_period) *params->min_period = min_period;

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_get_buffer_size( void *args )
{
    struct get_buffer_size_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    sigset_t saved;

    ps5_enter( &saved );

    *params->frames = stream->bufsize_frames;

    return leave_result( &saved, &params->result, S_OK );
}

static NTSTATUS ps5_get_latency( void *args )
{
    struct get_latency_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    sigset_t saved;

    ps5_enter( &saved );

    /* a period, plus the grain the port holds while the one before it plays */
    *params->latency = stream->period + (REFERENCE_TIME)PS5_GRAIN * 10000000 / PS5_RATE;

    return leave_result( &saved, &params->result, S_OK );
}

static NTSTATUS ps5_get_current_padding( void *args )
{
    struct get_current_padding_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    sigset_t saved;

    ps5_enter( &saved );

    *params->padding = stream->held_frames;

    return leave_result( &saved, &params->result, S_OK );
}

static NTSTATUS ps5_get_next_packet_size( void *args )
{
    struct get_next_packet_size_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    sigset_t saved;

    ps5_enter( &saved );

    *params->frames = stream->held_frames < stream->period_frames ? 0 : stream->period_frames;

    return leave_result( &saved, &params->result, S_OK );
}

static NTSTATUS ps5_get_frequency( void *args )
{
    struct get_frequency_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    sigset_t saved;

    ps5_enter( &saved );

    if (stream->share == AUDCLNT_SHAREMODE_SHARED)
        *params->freq = (UINT64)stream->fmt->nSamplesPerSec * stream->fmt->nBlockAlign;
    else
        *params->freq = stream->fmt->nSamplesPerSec;

    return leave_result( &saved, &params->result, S_OK );
}

static NTSTATUS ps5_get_position( void *args )
{
    struct get_position_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    UINT64 *pos = params->pos, *qpctime = params->qpctime;
    sigset_t saved;

    if (params->device)
    {
        FIXME( "Device position reporting not implemented\n" );
        params->result = E_NOTIMPL;
        return STATUS_SUCCESS;
    }

    ps5_enter( &saved );

    /* what the mixer has taken from the ring */
    *pos = stream->written_frames - stream->held_frames;
    if (*pos < stream->last_pos_frames) *pos = stream->last_pos_frames;
    stream->last_pos_frames = *pos;

    if (stream->share == AUDCLNT_SHAREMODE_SHARED) *pos *= stream->fmt->nBlockAlign;

    if (qpctime)
    {
        LARGE_INTEGER stamp, freq;
        NtQueryPerformanceCounter( &stamp, &freq );
        *qpctime = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
    }

    return leave_result( &saved, &params->result, S_OK );
}

static NTSTATUS ps5_set_volumes( void *args )
{
    struct set_volumes_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    unsigned int i;
    sigset_t saved;

    ps5_enter( &saved );
    for (i = 0; i < stream->channels; i++)
    {
        float volume = params->master_volume * params->volumes[i] * params->session_volumes[i];

        if (!(volume > 0.0f)) volume = 0.0f;
        else if (volume > 1.0f) volume = 1.0f;
        stream->left[i] = stream->weight[i][0] * volume;
        stream->right[i] = stream->weight[i][1] * volume;
    }
    ps5_leave( &saved );

    return STATUS_SUCCESS;
}

static NTSTATUS ps5_set_event_handle( void *args )
{
    struct set_event_handle_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    sigset_t saved;

    ps5_enter( &saved );

    if (!(stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK))
        return leave_result( &saved, &params->result, AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED );

    if (stream->event)
    {
        FIXME( "called twice\n" );
        return leave_result( &saved, &params->result, HRESULT_FROM_WIN32(ERROR_INVALID_NAME) );
    }

    stream->event = params->event;

    return leave_result( &saved, &params->result, S_OK );
}

/* IAudioClockAdjustment: the same frames played at another rate. */
static NTSTATUS ps5_set_sample_rate( void *args )
{
    struct set_sample_rate_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    sigset_t saved;

    ps5_enter( &saved );

    if (!(stream->flags & AUDCLNT_STREAMFLAGS_RATEADJUST))
        return leave_result( &saved, &params->result, AUDCLNT_E_UNSUPPORTED_FORMAT );

    if (!(params->rate >= 1000.0f && params->rate <= 384000.0f))
        return leave_result( &saved, &params->result, E_INVALIDARG );

    stream->step = (UINT64)((double)params->rate * 4294967296.0 / PS5_RATE);

    return leave_result( &saved, &params->result, S_OK );
}

static NTSTATUS ps5_is_started( void *args )
{
    struct is_started_params *params = args;
    struct ps5_stream *stream = handle_get_stream( params->stream );
    sigset_t saved;

    ps5_enter( &saved );

    return leave_result( &saved, &params->result, stream->playing ? S_OK : S_FALSE );
}

static NTSTATUS ps5_get_prop_value( void *args )
{
    struct get_prop_value_params *params = args;

    if (is_device( params->device, params->flow ) &&
        IsEqualPropertyKey( *params->prop, physical_speakers_key ))
    {
        params->value->vt = VT_UI4;
        params->value->ulVal = KSAUDIO_SPEAKER_STEREO;
        params->result = S_OK;
    }
    else params->result = E_NOTIMPL;

    return STATUS_SUCCESS;
}

/* No MIDI and no auxiliary devices: every count is 0, every device is a bad one.
 * mmdevapi sends these to the audio driver when midi_get_driver names no other. */
static UINT no_device( UINT msg, UINT count_msg )
{
    switch (msg)
    {
    case DRVM_INIT:
    case DRVM_EXIT:
    case DRVM_ENABLE:
    case DRVM_DISABLE:
        return MMSYSERR_NOERROR;
    }
    return msg == count_msg ? 0 : MMSYSERR_BADDEVICEID;
}

static NTSTATUS ps5_midi_out_message( void *args )
{
    struct midi_out_message_params *params = args;

    params->notify->send_notify = FALSE;
    *params->err = no_device( params->msg, MODM_GETNUMDEVS );
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_midi_in_message( void *args )
{
    struct midi_in_message_params *params = args;

    params->notify->send_notify = FALSE;
    *params->err = no_device( params->msg, MIDM_GETNUMDEVS );
    return STATUS_SUCCESS;
}

/* Nothing ever notifies: mmdevapi's notify thread may end at once. */
static NTSTATUS ps5_midi_notify_wait( void *args )
{
    struct midi_notify_wait_params *params = args;

    *params->quit = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS ps5_aux_message( void *args )
{
    struct aux_message_params *params = args;

    *params->err = no_device( params->msg, AUXDM_GETNUMDEVS );
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    ps5_process_attach,
    ps5_not_implemented,            /* process_detach: must not wait for anything, the title is ending */
    ps5_not_implemented,            /* main_loop_start */
    ps5_not_implemented,            /* main_loop_stop */
    ps5_get_endpoint_ids,
    ps5_create_stream,
    ps5_release_stream,
    ps5_start,
    ps5_stop,
    ps5_reset,
    ps5_get_render_buffer,
    ps5_release_render_buffer,
    ps5_get_capture_buffer,
    ps5_release_capture_buffer,
    ps5_is_format_supported,
    ps5_not_implemented,            /* get_loopback_capture_device: mmdevapi presets E_NOTIMPL */
    ps5_get_mix_format,
    ps5_get_device_period,
    ps5_get_buffer_size,
    ps5_get_latency,
    ps5_get_current_padding,
    ps5_get_next_packet_size,
    ps5_get_frequency,
    ps5_get_position,
    ps5_set_volumes,
    ps5_set_event_handle,
    ps5_set_sample_rate,
    ps5_test_connect,
    ps5_is_started,
    ps5_get_prop_value,
    ps5_not_implemented,            /* midi_get_driver: none, so the MIDI calls below come here */
    ps5_not_implemented,            /* midi_init: mmdevapi presets DRV_SUCCESS */
    ps5_not_implemented,            /* midi_release */
    ps5_midi_out_message,
    ps5_midi_in_message,
    ps5_midi_notify_wait,
    ps5_aux_message,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == funcs_count );
