/* Windows audio through the default render endpoint: three short tones. MIT license.
 *
 *   1. WASAPI, the endpoint's own mix format, event-driven: 440 Hz for 1 s,
 *      louder on the left (0.25 of full scale) than on the right (0.125).
 *   2. WASAPI, 44.1 kHz 16-bit mono with AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
 *      polled (the path dsound and xaudio2 use): 660 Hz for 0.5 s.
 *   3. winmm waveOut, 22.05 kHz 8-bit mono: 880 Hz for 0.3 s.
 *
 * Every call's HRESULT or MMRESULT is printed. Nothing here listens: the
 * checks are that each call succeeds, that frames are taken at the rate of a
 * clock (a tone of 1 s plays out in about 1 s) and that the stream position
 * ends at what was written. A person confirms the sound: three beeps, each
 * higher and shorter than the one before.
 *
 * With the arguments "stress [seconds]" (default 10) it plays no tones: four
 * threads open, fill, start, stop, reset and release streams of every format
 * the audio engine converts (8 to 32 bits and float, 1 to 8 channels, 8 to
 * 192 kHz), at once and in no order, quietly. It checks that no call fails
 * and that nothing hangs or crashes: it is for the driver's locking.
 *
 * With the argument "exit" it plays a quiet tone, and while one thread is
 * busy filling the stream the main thread does what ExitProcess does first:
 * it has Windows end every other thread of the process (NtTerminateProcess
 * with no handle). Then it stops and releases the stream, as a sound
 * library's destructor would at process detach, and exits. The check is that
 * the program ends: a driver thread ended while it held the driver's lock
 * would leave the main thread waiting for ever.
 *
 * Exit status: the number of failed checks; 98 if the watchdog fired (30 s,
 * or 20 s after a stress run should have ended). "exit" ends with 0 or hangs.
 */
#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const GUID float_subtype = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID pcm_subtype = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const double two_pi = 6.283185307179586;
static int failures;

static HRESULT step(const char *what, HRESULT hr)
{
    printf("%s %s hr=0x%08lx\n", SUCCEEDED(hr) ? "ok  " : "FAIL", what, (unsigned long)hr);
    fflush(stdout);
    if (FAILED(hr)) failures++;
    return hr;
}
static MMRESULT mm_step(const char *what, MMRESULT result)
{
    printf("%s %s mmresult=%u\n", result == MMSYSERR_NOERROR ? "ok  " : "FAIL", what, result);
    fflush(stdout);
    if (result != MMSYSERR_NOERROR) failures++;
    return result;
}
static void check(const char *what, int passed)
{
    printf("%s %s\n", passed ? "ok  " : "FAIL", what);
    fflush(stdout);
    if (!passed) failures++;
}
static double seconds(void)
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return (double)now.QuadPart / (double)frequency.QuadPart;
}
static DWORD watchdog_ms = 30000;
static DWORD WINAPI watchdog(void *unused)
{
    (void)unused;
    Sleep(watchdog_ms);
    printf("FAIL watchdog: the test did not end in %lu s\n", (unsigned long)watchdog_ms / 1000);
    fflush(stdout);
    ExitProcess(98);
    return 0;
}

/* frames of a sine into a buffer of the given format; left and right amplitudes for stereo */
static int fill(BYTE *data, const WAVEFORMATEX *format, UINT32 first, UINT32 frames, double hertz, double left, double right)
{
    const WAVEFORMATEXTENSIBLE *extensible = (const WAVEFORMATEXTENSIBLE *)format;
    int is_float = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(&extensible->SubFormat, &float_subtype));
    int is_pcm = format->wFormatTag == WAVE_FORMAT_PCM ||
        (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(&extensible->SubFormat, &pcm_subtype));
    if (!(is_float && format->wBitsPerSample == 32) && !(is_pcm && (format->wBitsPerSample == 16 || format->wBitsPerSample == 8)))
        return 0;
    for (UINT32 i = 0; i < frames; i++)
    {
        double wave = sin(two_pi * hertz * (double)(first + i) / (double)format->nSamplesPerSec);
        for (unsigned channel = 0; channel < format->nChannels; channel++)
        {
            double value = wave * (channel == 0 ? left : channel == 1 ? right : 0.0);
            BYTE *at = data + (size_t)i * format->nBlockAlign + channel * (format->wBitsPerSample / 8);
            if (is_float) { float sample = (float)value; memcpy(at, &sample, sizeof(sample)); }
            else if (format->wBitsPerSample == 16) { short sample = (short)(value * 32767.0); memcpy(at, &sample, sizeof(sample)); }
            else *at = (BYTE)(128.0 + value * 127.0);
        }
    }
    return 1;
}

/* Write the tone as the stream makes room, then wait for it to play out. event: wait on it; else poll. */
static void play(const char *name, IAudioClient *client, const WAVEFORMATEX *format, HANDLE event,
                 double hertz, double tone_seconds, double left, double right)
{
    IAudioRenderClient *render = NULL;
    IAudioClock *clock = NULL;
    UINT32 buffer_frames = 0, padding = 0, written = 0, waits = 0, timeouts = 0;
    const UINT32 total = (UINT32)(format->nSamplesPerSec * tone_seconds);
    UINT64 clock_frequency = 0, position = 0;
    REFERENCE_TIME latency = 0;
    char line[160];

    if (FAILED(step("IAudioClient::GetBufferSize", IAudioClient_GetBufferSize(client, &buffer_frames)))) return;
    if (FAILED(step("IAudioClient::GetService(IAudioRenderClient)", IAudioClient_GetService(client, &IID_IAudioRenderClient, (void **)&render)))) return;
    step("IAudioClient::GetService(IAudioClock)", IAudioClient_GetService(client, &IID_IAudioClock, (void **)&clock));
    step("IAudioClient::GetStreamLatency", IAudioClient_GetStreamLatency(client, &latency));
    printf("info %s: buffer %u frames, latency %ld00 ns, tone %u frames\n", name, buffer_frames, (long)latency, total);
    if (FAILED(step("IAudioClient::Start", IAudioClient_Start(client)))) { IAudioRenderClient_Release(render); return; }

    const double began = seconds();
    HRESULT buffer_result = S_OK;
    while (written < total && seconds() - began < tone_seconds + 4.0)
    {
        if (event)
        {
            if (WaitForSingleObject(event, 500) != WAIT_OBJECT_0) { timeouts++; continue; }
        }
        else Sleep(5);
        waits++;
        if (FAILED(buffer_result = IAudioClient_GetCurrentPadding(client, &padding))) break;
        UINT32 room = buffer_frames - padding, frames = room < total - written ? room : total - written;
        BYTE *data = NULL;
        if (!frames) continue;
        if (FAILED(buffer_result = IAudioRenderClient_GetBuffer(render, frames, &data))) break;
        if (!fill(data, format, written, frames, hertz, left, right)) { IAudioRenderClient_ReleaseBuffer(render, 0, 0); buffer_result = E_NOTIMPL; break; }
        if (FAILED(buffer_result = IAudioRenderClient_ReleaseBuffer(render, frames, 0))) break;
        written += frames;
    }
    step("GetCurrentPadding/GetBuffer/ReleaseBuffer, every call while writing the tone", buffer_result);
    /* the frames still held play out: at most the buffer's length, with a margin */
    for (int i = 0; i < 400; i++)
    {
        if (FAILED(IAudioClient_GetCurrentPadding(client, &padding)) || !padding) break;
        Sleep(5);
    }
    const double took = seconds() - began;
    if (clock)
    {
        step("IAudioClock::GetFrequency", IAudioClock_GetFrequency(clock, &clock_frequency));
        step("IAudioClock::GetPosition", IAudioClock_GetPosition(clock, &position, NULL));
    }
    step("IAudioClient::Stop", IAudioClient_Stop(client));
    const UINT64 played = clock_frequency ? position * format->nSamplesPerSec / clock_frequency : 0;
    printf("info %s: wrote %u of %u frames in %u %s (%u timeouts); played out in %.0f ms; position %lu frames; %u frames left\n",
        name, written, total, waits, event ? "events" : "polls", timeouts, took * 1000.0, (unsigned long)played, padding);
    snprintf(line, sizeof(line), "%s: all frames were written", name);
    check(line, written == total);
    snprintf(line, sizeof(line), "%s: the stream took frames at the rate of a clock (%.0f ms for %.0f ms of sound)", name, took * 1000.0, tone_seconds * 1000.0);
    check(line, took > tone_seconds * 0.85 && took < tone_seconds + 0.6);
    snprintf(line, sizeof(line), "%s: everything written was played and the position says so", name);
    check(line, !padding && played == written);
    if (event) { snprintf(line, sizeof(line), "%s: the event was set for every period", name); check(line, !timeouts); }
    if (clock) IAudioClock_Release(clock);
    IAudioRenderClient_Release(render);
}

static void wave_out(void)
{
    enum { rate = 22050, frames = rate * 3 / 10 };
    static BYTE samples[frames];
    WAVEFORMATEX format = { WAVE_FORMAT_PCM, 1, rate, rate, 1, 8, 0 };
    WAVEHDR header;
    HWAVEOUT device = NULL;

    printf("info winmm: waveOutGetNumDevs=%u midiOutGetNumDevs=%u midiInGetNumDevs=%u auxGetNumDevs=%u\n",
        waveOutGetNumDevs(), midiOutGetNumDevs(), midiInGetNumDevs(), auxGetNumDevs());
    check("winmm: there is a wave output device", waveOutGetNumDevs() >= 1);
    if (mm_step("waveOutOpen(WAVE_MAPPER, 22050 Hz 8-bit mono)", waveOutOpen(&device, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL))) return;
    fill(samples, &format, 0, frames, 880.0, 0.25, 0.25);
    memset(&header, 0, sizeof(header));
    header.lpData = (char *)samples;
    header.dwBufferLength = frames;
    if (!mm_step("waveOutPrepareHeader", waveOutPrepareHeader(device, &header, sizeof(header))))
    {
        const double began = seconds();
        if (!mm_step("waveOutWrite", waveOutWrite(device, &header, sizeof(header))))
        {
            while (!(header.dwFlags & WHDR_DONE) && seconds() - began < 4.0) Sleep(5);
            const double took = seconds() - began;
            printf("info winmm: the buffer was %s after %.0f ms (300 ms of sound)\n", header.dwFlags & WHDR_DONE ? "done" : "NOT done", took * 1000.0);
            check("winmm: the buffer played out at the rate of a clock", (header.dwFlags & WHDR_DONE) && took > 0.2 && took < 1.2);
        }
        if (!(header.dwFlags & WHDR_DONE)) waveOutReset(device);
        mm_step("waveOutUnprepareHeader", waveOutUnprepareHeader(device, &header, sizeof(header)));
    }
    mm_step("waveOutClose", waveOutClose(device));
}

/* ------------------------------------------------------------------ stress */

static IMMDevice *stress_device;
static double stress_until;
static LONG stress_failures, stress_streams, stress_calls;

static unsigned random_next(unsigned *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}
static HRESULT stress_call(const char *what, HRESULT hr)
{
    InterlockedIncrement(&stress_calls);
    if (FAILED(hr) && InterlockedIncrement(&stress_failures) <= 20)
    {
        printf("FAIL stress: %s hr=0x%08lx\n", what, (unsigned long)hr);
        fflush(stdout);
    }
    return hr;
}
static DWORD WINAPI stress_thread(void *argument)
{
    static const unsigned rates[] = {8000, 11025, 22050, 44100, 48000, 96000, 192000};
    static const unsigned channel_counts[] = {1, 2, 2, 4, 6, 8};
    static const DWORD masks[9] = {0, 0x4, 0x3, 0, 0x33, 0, 0x3f, 0, 0x63f};
    static const unsigned widths[] = {8, 16, 24, 32, 32};      /* the last is float */
    unsigned seed = 12345u + 7919u * (unsigned)(UINT_PTR)argument;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    while (seconds() < stress_until)
    {
        IAudioClient *client = NULL;
        IAudioRenderClient *render = NULL;
        IAudioClock *clock = NULL;
        ISimpleAudioVolume *volume = NULL;
        WAVEFORMATEXTENSIBLE format;
        const unsigned kind = random_next(&seed) % 5, channels = channel_counts[random_next(&seed) % 6];
        const unsigned rate = rates[random_next(&seed) % 7], use_event = random_next(&seed) & 1;
        const int plain = channels <= 2 && widths[kind] != 24 && (random_next(&seed) & 1);
        UINT32 buffer_frames = 0, padding = 0;
        HANDLE event = NULL;
        int started = 0;

        memset(&format, 0, sizeof(format));
        format.Format.wFormatTag = plain ? (kind == 4 ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM) : WAVE_FORMAT_EXTENSIBLE;
        format.Format.nChannels = (WORD)channels;
        format.Format.nSamplesPerSec = rate;
        format.Format.wBitsPerSample = (WORD)widths[kind];
        format.Format.nBlockAlign = (WORD)(channels * widths[kind] / 8);
        format.Format.nAvgBytesPerSec = rate * format.Format.nBlockAlign;
        format.Format.cbSize = plain ? 0 : sizeof(format) - sizeof(format.Format);
        format.Samples.wValidBitsPerSample = (WORD)widths[kind];
        format.dwChannelMask = masks[channels];
        format.SubFormat = kind == 4 ? float_subtype : pcm_subtype;

        if (FAILED(stress_call("Activate", IMMDevice_Activate(stress_device, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (void **)&client)))) break;
        if (SUCCEEDED(stress_call("Initialize", IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | (use_event ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0),
                (REFERENCE_TIME)(300000 + random_next(&seed) % 4700000), 0, &format.Format, NULL))))
        {
            InterlockedIncrement(&stress_streams);
            if (use_event)
            {
                event = CreateEventW(NULL, FALSE, FALSE, NULL);
                stress_call("SetEventHandle", IAudioClient_SetEventHandle(client, event));
            }
            stress_call("GetBufferSize", IAudioClient_GetBufferSize(client, &buffer_frames));
            stress_call("GetService(render)", IAudioClient_GetService(client, &IID_IAudioRenderClient, (void **)&render));
            stress_call("GetService(clock)", IAudioClient_GetService(client, &IID_IAudioClock, (void **)&clock));
            stress_call("GetService(volume)", IAudioClient_GetService(client, &IID_ISimpleAudioVolume, (void **)&volume));
            for (unsigned turn = 5 + random_next(&seed) % 36; render && turn && seconds() < stress_until; turn--)
            {
                const unsigned action = random_next(&seed) % 16;
                BYTE *data = NULL;
                UINT64 position = 0;

                if (action < 9)
                {
                    stress_call("GetCurrentPadding", IAudioClient_GetCurrentPadding(client, &padding));
                    UINT32 room = buffer_frames > padding ? buffer_frames - padding : 0;
                    UINT32 frames = room ? 1 + random_next(&seed) % room : 0;
                    if (frames && SUCCEEDED(stress_call("GetBuffer", IAudioRenderClient_GetBuffer(render, frames, &data))))
                    {
                        /* quiet noise in whatever the sample format is: every byte pattern must be playable */
                        UINT32 keep = random_next(&seed) % 4 ? frames : random_next(&seed) % (frames + 1);
                        const int silent = !(random_next(&seed) % 8);
                        for (UINT32 i = 0; i < keep * format.Format.nBlockAlign; i++) data[i] = (BYTE)(random_next(&seed) % 3);
                        if (kind == 4) for (UINT32 i = 0; i < keep * channels; i++)
                        {
                            float sample = ((int)(random_next(&seed) % 2001) - 1000) / 50000.0f;
                            memcpy(data + i * 4, &sample, 4);
                        }
                        stress_call("ReleaseBuffer", IAudioRenderClient_ReleaseBuffer(render, keep, silent ? AUDCLNT_BUFFERFLAGS_SILENT : 0));
                    }
                }
                else if (action < 11 && !started) started = SUCCEEDED(stress_call("Start", IAudioClient_Start(client)));
                else if (action == 11 && started) { stress_call("Stop", IAudioClient_Stop(client)); started = 0; }
                else if (action == 12 && !started) stress_call("Reset", IAudioClient_Reset(client));
                else if (action == 13 && volume) stress_call("SetMasterVolume", ISimpleAudioVolume_SetMasterVolume(volume, (float)(random_next(&seed) % 101) / 100.0f, NULL));
                else if (action == 14 && clock) stress_call("GetPosition", IAudioClock_GetPosition(clock, &position, NULL));
                else if (event) WaitForSingleObject(event, started ? 30 : 1);
                else Sleep(random_next(&seed) % 12);
            }
            /* some streams are released while they play */
            if (started && (random_next(&seed) & 1)) stress_call("Stop", IAudioClient_Stop(client));
        }
        if (volume) ISimpleAudioVolume_Release(volume);
        if (clock) IAudioClock_Release(clock);
        if (render) IAudioRenderClient_Release(render);
        IAudioClient_Release(client);
        if (event) CloseHandle(event);
    }
    CoUninitialize();
    return 0;
}
static int stress(double run_seconds)
{
    IMMDeviceEnumerator *enumerator = NULL;
    HANDLE threads[4];

    printf("WoWPS5 Win32 audio stress, %.0f s\n", run_seconds);
    if (FAILED(step("CoInitializeEx(COINIT_MULTITHREADED)", CoInitializeEx(NULL, COINIT_MULTITHREADED)))) return 1;
    if (FAILED(step("CoCreateInstance(MMDeviceEnumerator)", CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
            &IID_IMMDeviceEnumerator, (void **)&enumerator)))) return 1;
    if (FAILED(step("IMMDeviceEnumerator::GetDefaultAudioEndpoint(eRender, eConsole)",
            IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &stress_device)))) return 1;
    stress_until = seconds() + run_seconds;
    for (int i = 0; i < 4; i++) threads[i] = CreateThread(NULL, 0, stress_thread, (void *)(UINT_PTR)i, 0, NULL);
    WaitForMultipleObjects(4, threads, TRUE, INFINITE);
    printf("info stress: %ld streams, %ld calls, %ld failed\n", (long)stress_streams, (long)stress_calls, (long)stress_failures);
    check("stress: streams were opened and no call failed", stress_streams > 0 && !stress_failures);
    IMMDevice_Release(stress_device);
    IMMDeviceEnumerator_Release(enumerator);
    printf(failures ? "WoWPS5 Win32 audio stress FAIL (%d)\n" : "WoWPS5 Win32 audio stress PASS\n", failures);
    fflush(stdout);
    return failures;
}

/* -------------------------------------------------------------------- exit */

static IAudioClient *exit_client;
static IAudioRenderClient *exit_render;
static UINT32 exit_buffer_frames;
static WAVEFORMATEX *exit_format;

/* not through the C runtime: its locks may belong to a thread that was ended */
static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, (DWORD)strlen(text), &written, NULL);
}
NTSTATUS NTAPI NtTerminateProcess(HANDLE process, LONG status);

static DWORD WINAPI exit_filler(void *unused)
{
    UINT32 padding, position = 0;
    (void)unused;
    for (;;)        /* until Windows ends this thread, most likely inside one of these calls */
    {
        BYTE *data;
        if (FAILED(IAudioClient_GetCurrentPadding(exit_client, &padding))) continue;
        UINT32 room = exit_buffer_frames - padding, frames = room < 32 ? room : 32;
        if (!frames || FAILED(IAudioRenderClient_GetBuffer(exit_render, frames, &data))) continue;
        fill(data, exit_format, position, frames, 440.0, 0.05, 0.05);
        IAudioRenderClient_ReleaseBuffer(exit_render, frames, 0);
        position += frames;
    }
    return 0;
}
static int exit_while_playing(void)
{
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice *device = NULL;
    HANDLE event;

    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)) ||
        FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER, &IID_IMMDeviceEnumerator, (void **)&enumerator)) ||
        FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device)) ||
        FAILED(IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (void **)&exit_client)) ||
        FAILED(IAudioClient_GetMixFormat(exit_client, &exit_format)) ||
        FAILED(IAudioClient_Initialize(exit_client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 500000, 0, exit_format, NULL)) ||
        !(event = CreateEventW(NULL, FALSE, FALSE, NULL)) || FAILED(IAudioClient_SetEventHandle(exit_client, event)) ||
        FAILED(IAudioClient_GetBufferSize(exit_client, &exit_buffer_frames)) ||
        FAILED(IAudioClient_GetService(exit_client, &IID_IAudioRenderClient, (void **)&exit_render)) ||
        FAILED(IAudioClient_Start(exit_client)))
    {
        printf("FAIL exit: no playing stream\n");
        exit_client = NULL;
        return 1;
    }
    CloseHandle(CreateThread(NULL, 0, exit_filler, NULL, 0, NULL));
    Sleep(60 + GetTickCount() % 90);
    /* every other thread ends where it stands: the filler, and the driver's own */
    NtTerminateProcess(NULL, 0);
    Sleep(GetTickCount() % 20);
    IAudioClient_Stop(exit_client);
    IAudioRenderClient_Release(exit_render);
    IAudioClient_Release(exit_client);
    say("ok   exit: the stream was stopped and released after the other threads were ended\n");
    ExitProcess(0);
    return 0;
}

int main(int argc, char **argv)
{
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDeviceCollection *collection = NULL;
    IMMDevice *device = NULL, *capture = NULL;
    IAudioClient *client = NULL;
    WAVEFORMATEX *mix = NULL, *closest = NULL;
    REFERENCE_TIME default_period = 0, minimum_period = 0;
    WCHAR *id = NULL;
    UINT count = 0;

    if (argc > 1 && !strcmp(argv[1], "stress"))
    {
        double run_seconds = argc > 2 ? atof(argv[2]) : 10.0;
        if (!(run_seconds >= 1.0 && run_seconds <= 600.0)) run_seconds = 10.0;
        watchdog_ms = (DWORD)(run_seconds * 1000.0) + 20000;
        CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
        return stress(run_seconds);
    }
    if (argc > 1 && !strcmp(argv[1], "exit")) return exit_while_playing();   /* no watchdog: a hang is the finding */
    CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    printf("WoWPS5 Win32 audio test\n");
    if (FAILED(step("CoInitializeEx(COINIT_MULTITHREADED)", CoInitializeEx(NULL, COINIT_MULTITHREADED)))) goto done;
    if (FAILED(step("CoCreateInstance(MMDeviceEnumerator)", CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
            &IID_IMMDeviceEnumerator, (void **)&enumerator)))) goto done;
    if (SUCCEEDED(step("IMMDeviceEnumerator::EnumAudioEndpoints(eRender, active)",
            IMMDeviceEnumerator_EnumAudioEndpoints(enumerator, eRender, DEVICE_STATE_ACTIVE, &collection))))
    {
        IMMDeviceCollection_GetCount(collection, &count);
        printf("info %u active render endpoint(s)\n", count);
        IMMDeviceCollection_Release(collection);
    }
    /* expected to fail where there is no microphone: reported, not counted */
    HRESULT capture_result = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eCapture, eConsole, &capture);
    printf("info GetDefaultAudioEndpoint(eCapture) hr=0x%08lx (%s)\n", (unsigned long)capture_result,
        SUCCEEDED(capture_result) ? "a capture endpoint" : "no capture endpoint");
    if (capture) IMMDevice_Release(capture);
    if (FAILED(step("IMMDeviceEnumerator::GetDefaultAudioEndpoint(eRender, eConsole)",
            IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device)))) goto done;
    if (SUCCEEDED(step("IMMDevice::GetId", IMMDevice_GetId(device, &id)))) { printf("info endpoint id %ls\n", id); CoTaskMemFree(id); }

    /* 1. the mix format, event-driven */
    if (FAILED(step("IMMDevice::Activate(IAudioClient)", IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (void **)&client)))) goto done;
    if (FAILED(step("IAudioClient::GetMixFormat", IAudioClient_GetMixFormat(client, &mix)))) goto done;
    printf("info mix format: tag 0x%04x, %u channels, %lu Hz, %u bits, block %u, extra %u bytes\n", mix->wFormatTag, mix->nChannels,
        (unsigned long)mix->nSamplesPerSec, mix->wBitsPerSample, mix->nBlockAlign, mix->cbSize);
    step("IAudioClient::GetDevicePeriod", IAudioClient_GetDevicePeriod(client, &default_period, &minimum_period));
    printf("info device period: default %ld00 ns, minimum %ld00 ns\n", (long)default_period, (long)minimum_period);
    step("IAudioClient::IsFormatSupported(shared, mix format)", IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_SHARED, mix, &closest));
    if (closest) { CoTaskMemFree(closest); closest = NULL; }
    if (SUCCEEDED(step("IAudioClient::Initialize(shared, EVENTCALLBACK, 100 ms, mix format)",
            IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 1000000, 0, mix, NULL))))
    {
        HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (SUCCEEDED(step("IAudioClient::SetEventHandle", IAudioClient_SetEventHandle(client, event))))
            play("tone 1 (440 Hz, 1 s, mix format, events)", client, mix, event, 440.0, 1.0, 0.25, 0.125);
        IAudioClient_Release(client); client = NULL;
        CloseHandle(event);
    }
    if (client) { IAudioClient_Release(client); client = NULL; }
    Sleep(300);

    /* 2. another rate, width and channel count, converted by the audio engine; polled */
    if (SUCCEEDED(step("IMMDevice::Activate(IAudioClient), second client", IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (void **)&client))))
    {
        WAVEFORMATEX pcm = { WAVE_FORMAT_PCM, 1, 44100, 44100 * 2, 2, 16, 0 };
        HRESULT supported = IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_SHARED, &pcm, &closest);
        printf("info IsFormatSupported(shared, 44100 Hz 16-bit mono) hr=0x%08lx%s\n", (unsigned long)supported,
            supported == S_FALSE ? " (S_FALSE: a closest match is offered, as on Windows)" : "");
        if (closest) { CoTaskMemFree(closest); closest = NULL; }
        if (SUCCEEDED(step("IAudioClient::Initialize(shared, AUTOCONVERTPCM, 200 ms, 44100 Hz 16-bit mono)",
                IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 2000000, 0, &pcm, NULL))))
            play("tone 2 (660 Hz, 0.5 s, 44.1 kHz mono PCM, polled)", client, &pcm, NULL, 660.0, 0.5, 0.25, 0.25);
        IAudioClient_Release(client); client = NULL;
    }
    Sleep(300);

    /* 3. winmm */
    wave_out();
    Sleep(200);

done:
    if (mix) CoTaskMemFree(mix);
    if (client) IAudioClient_Release(client);
    if (device) IMMDevice_Release(device);
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);
    printf(failures ? "WoWPS5 Win32 audio test FAIL (%d)\n" : "WoWPS5 Win32 audio test PASS\n", failures);
    fflush(stdout);
    return failures;
}
