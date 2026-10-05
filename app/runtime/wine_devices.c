/* Native PS5 devices -> Wine hardware events and XInput. SPDX-License-Identifier: MIT
 * Interoperability layouts checked against PS5 native input research and AnyPad;
 * no pairing or virtual-device manipulation is needed to read a title's devices. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

struct touch { uint16_t x, y; uint8_t id, reserved[3]; };
struct pad_data {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry, l2, r2, reserved[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touches, reserved1[3]; uint32_t reserved2;
    struct touch touch[2];
    uint8_t connected, reserved3[3]; uint64_t timestamp;
    uint8_t extension[16], connected_count, reserved4[15];
};
struct keyboard_data {
    uint64_t timestamp; uint8_t intercepted, reserved[7], connected, reserved1[3];
    int32_t length; uint32_t leds, modifiers; uint16_t keys[16]; uint8_t reserved2[32];
};
struct mouse_data {
    uint64_t timestamp; uint8_t connected, reserved[3]; uint32_t buttons;
    int32_t x, y, wheel, tilt; uint8_t reserved1[8];
};
struct gamepad { uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; };
struct gamepad_state { uint32_t packet; struct gamepad pad; };
_Static_assert(sizeof(struct pad_data) == 120 && offsetof(struct pad_data, touch) == 60, "pad ABI");
_Static_assert(sizeof(struct keyboard_data) == 96 && offsetof(struct keyboard_data, keys) == 32, "keyboard ABI");
_Static_assert(sizeof(struct mouse_data) == 40 && offsetof(struct mouse_data, x) == 16, "mouse ABI");
_Static_assert(sizeof(struct gamepad_state) == 16, "XInput ABI");

extern int wowps5_input_queue(unsigned, unsigned, unsigned, int, int);
static pthread_mutex_t devices_lock = PTHREAD_MUTEX_INITIALIZER;
static struct gamepad_state gamepad;
static int pad_connected, pad_enabled = 1;
static uint8_t keyboard_held[8][256], key_held[256];
static uint32_t mouse_held[9], buttons_held;
static int touch_active, touch_x, touch_y, touch_click;
static uint8_t touch_id;
static unsigned repeat_key;
static uint64_t repeat_at;
static uint64_t pad_records, touch_moves, keyboard_edges, mouse_moves;

static uint64_t milliseconds(void)
{
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static unsigned key_vk(unsigned usage)
{
    static const uint8_t punctuation[] = {0x0d,0x1b,0x08,0x09,0x20,0xbd,0xbb,0xdb,0xdd,0xdc,0xdc,0xba,0xde,0xc0,0xbc,0xbe,0xbf,0x14};
    static const uint8_t navigation[] = {0x2c,0x91,0x13,0x2d,0x24,0x21,0x2e,0x23,0x22,0x27,0x25,0x28,0x26,0x90,0x6f,0x6a,0x6d,0x6b,0x0d};
    static const uint8_t modifiers[] = {0xa2,0xa0,0xa4,0x5b,0xa3,0xa1,0xa5,0x5c};
    if (usage >= 4 && usage <= 29) return 'A' + usage - 4;
    if (usage >= 30 && usage <= 38) return '1' + usage - 30;
    if (usage == 39) return '0';
    if (usage >= 40 && usage <= 57) return punctuation[usage-40];
    if (usage >= 58 && usage <= 69) return 0x70 + usage - 58;
    if (usage >= 70 && usage <= 88) return navigation[usage-70];
    if (usage >= 89 && usage <= 97) return 0x61 + usage - 89;
    if (usage == 98) return 0x60;
    if (usage == 99) return 0x6e;
    if (usage == 100) return 0xe2;
    if (usage == 101) return 0x5d;
    if (usage >= 104 && usage <= 115) return 0x7c + usage - 104;
    if (usage >= 224 && usage <= 231) return modifiers[usage-224];
    return 0;
}
static unsigned key_flags(unsigned vk)
{
    return vk == 0xa3 || vk == 0xa5 || vk == 0x5b || vk == 0x5c ||
           (vk >= 0x21 && vk <= 0x2e) || vk == 0x6f ? 1 : 0;
}
static void keyboard_sample(unsigned device, const struct keyboard_data *sample)
{
    uint8_t held[256] = {0};
    if (sample->connected && !sample->intercepted)
    {
        /* A rollover/error report is not a release of all held keys. */
        for (unsigned i = 0; i < 16; i++) if (sample->keys[i] && sample->keys[i] < 4) return;
        for (unsigned i = 0; i < 8; i++) if (sample->modifiers & (1u << i)) held[key_vk(224+i)] = 1;
        for (unsigned i = 0; i < 16; i++) { unsigned vk = key_vk(sample->keys[i]); if (vk) held[vk] = 1; }
    }
    memcpy(keyboard_held[device], held, sizeof(held));
    /* Modifiers precede ordinary keys on both edges, keeping chords intact. */
    for (unsigned pass = 0; pass < 2; pass++) for (unsigned vk = 1; vk < 256; vk++)
    {
        int modifier = (vk >= 0xa0 && vk <= 0xa5) || vk == 0x5b || vk == 0x5c;
        if (modifier != (pass == 0)) continue;
        unsigned down = 0;
        for (unsigned i = 0; i < 8; i++) down |= keyboard_held[i][vk];
        if (down != key_held[vk] && wowps5_input_queue(1, vk, key_flags(vk) | (down ? 0 : 2), 0, 0))
        {
            key_held[vk] = down;
            keyboard_edges++;
            if (!modifier && down) { repeat_key = vk; repeat_at = milliseconds() + 500; }
            else if (!down && repeat_key == vk) repeat_key = 0;
        }
    }
}
static void mouse_buttons(unsigned device, unsigned held)
{
    static const unsigned down[] = {2,8,32,128,128}, up[] = {4,16,64,256,256};
    mouse_held[device] = held & 31;
    unsigned all = 0;
    for (unsigned i = 0; i < 9; i++) all |= mouse_held[i];
    for (unsigned i = 0; i < 5; i++) if ((all ^ buttons_held) & (1u << i))
        if (wowps5_input_queue(2, i < 3 ? 0 : i-2, all & (1u << i) ? down[i] : up[i], 0, 0))
            buttons_held = (buttons_held & ~(1u << i)) | (all & (1u << i));
}
static void mouse_sample(unsigned device, const struct mouse_data *sample)
{
    int usable = sample->connected && !(sample->buttons & 0x80000000u);
    if (usable && (sample->x || sample->y)) { wowps5_input_queue(2, 0, 1, sample->x, sample->y); mouse_moves++; }
    mouse_buttons(device, usable ? sample->buttons : 0);
    if (usable && sample->wheel) wowps5_input_queue(2, (uint32_t)(sample->wheel * 120), 0x800, 0, 0);
    if (usable && sample->tilt) wowps5_input_queue(2, (uint32_t)(sample->tilt * 120), 0x1000, 0, 0);
}
static int16_t axis(unsigned value, int invert)
{
    int n = (int)value - 128;
    /* Exact neutral, full signed range, native Y is down-positive. */
    if (n >= -1 && n <= 1) return 0;
    int result = n < 0 ? n * 256 : n * 32767 / 127;
    if (invert) result = result == -32768 ? 32767 : -result;
    return result;
}
/* --- on-screen keyboard: shortcut gate, system dialog, typing (wine_osk.c) --- */
#include "wine_osk.c"
static void pad_sample(const struct pad_data *sample)
{
    static const uint32_t native[] = {0x10,0x40,0x80,0x20,8,1,2,4,0x400,0x800,0x4000,0x2000,0x8000,0x1000};
    static const uint16_t xbox[] = {1,2,4,8,0x10,0x20,0x40,0x80,0x100,0x200,0x1000,0x2000,0x4000,0x8000};
    int usable = sample->connected && !(sample->buttons & 0x80000000u);
    if (usable) pad_records++;
    /* --- on-screen keyboard: a pad it owns is as one the system intercepted,
     * except that the game keeps it: connected and neutral. Otherwise the game
     * sees the record less the shortcut buttons being held back. --- */
    struct pad_data gated = *sample;
    int owned = osk_pad(usable, &gated.buttons, &gated.l2, &gated.r2);
    int connected = usable || (owned && sample->connected);
    sample = &gated;
    if (owned) usable = 0;
    /* --- end on-screen keyboard --- */
    struct gamepad next = {0};
    if (usable && pad_enabled)
    {
        for (unsigned i = 0; i < 14; i++) if (sample->buttons & native[i]) next.buttons |= xbox[i];
        next.lt = sample->l2; next.rt = sample->r2;
        next.lx = axis(sample->lx, 0); next.ly = axis(sample->ly, 1);
        next.rx = axis(sample->rx, 0); next.ry = axis(sample->ry, 1);
    }
    if (pad_connected != connected || memcmp(&next, &gamepad.pad, sizeof(next)))
    { gamepad.pad = next; if (!++gamepad.packet) ++gamepad.packet; }
    pad_connected = connected;
    if (usable && sample->touches)
    {
        const struct touch *t = &sample->touch[0];
        /* Lifting/replacing a finger rebases without moving the cursor. */
        if (touch_active && touch_id == t->id && (t->x != touch_x || t->y != touch_y))
        {
            wowps5_input_queue(2, 0, 1, ((int)t->x-touch_x)*2, ((int)t->y-touch_y)*2);
            touch_moves++;
        }
        touch_active = 1; touch_id = t->id; touch_x = t->x; touch_y = t->y;
    }
    else touch_active = 0;
    if (usable && (sample->buttons & 0x100000))
    {
        /* Latch the click side until release so dragging across the center
         * never switches buttons. Two fingers also request secondary click. */
        if (!touch_click) touch_click = sample->touches > 1 || (touch_active && touch_x >= 960) ? 2 : 1;
    }
    else touch_click = 0;
    mouse_buttons(8, touch_click);
}

int wowps5_gamepad_state(unsigned index, void *state)
{
    if (index >= 4 || !state) return 160;
    pthread_mutex_lock(&devices_lock);
    int result = index == 0 && pad_connected ? 0 : 1167;
    if (!result) memcpy(state, &gamepad, sizeof(gamepad));
    static int reported;
    if (!result && !reported) { fprintf(stderr, "[WoWPS5 input] Windows XInput reached native controller\n"); reported = 1; }
    pthread_mutex_unlock(&devices_lock);
    return result;
}

#ifndef WOWPS5_INPUT_TEST
int sceUserServiceInitialize(const void *);
int sceUserServiceGetInitialUser(int32_t *);
int scePadInit(void);
int scePadOpen(int32_t, int32_t, int32_t, const void *);
int scePadRead(int32_t, void *, int32_t);
int scePadSetVibration(int32_t, const void *);
int scePadSetVibrationMode(int32_t, int32_t);
int sceSysmoduleLoadModule(uint16_t);
int sceKeyboardInit(void);
int sceKeyboardOpen(int32_t, int32_t, int32_t, const void *);
int sceKeyboardRead(int32_t, void *, int32_t);
int sceMouseInit(void);
int sceMouseOpen(int32_t, int32_t, int32_t, const void *);
int sceMouseRead(int32_t, void *, int32_t);
static int pad_handle = -1;
struct peripheral {
    int (*init)(void);
    int (*open)(int32_t, int32_t, int32_t, const void *);
    int (*read)(int32_t, void *, int32_t);
    int handles[8];
};
static struct peripheral keyboard, mouse;
/* Keep calls through the native PLT. Function-address imports in the converted
 * executable can remain null even when an ordinary linked call resolves. */
static int keyboard_init(void) { return sceKeyboardInit(); }
static int keyboard_open(int32_t user, int32_t type, int32_t index, const void *p) { return sceKeyboardOpen(user,type,index,p); }
static int keyboard_read(int32_t handle, void *p, int32_t n) { return sceKeyboardRead(handle,p,n); }
static int mouse_init(void) { return sceMouseInit(); }
static int mouse_open(int32_t user, int32_t type, int32_t index, const void *p) { return sceMouseOpen(user,type,index,p); }
static int mouse_read(int32_t handle, void *p, int32_t n) { return sceMouseRead(handle,p,n); }

static void peripheral_open(struct peripheral *p, uint16_t id, const char *prefix, int32_t user)
{
    for (unsigned i = 0; i < 8; i++) p->handles[i] = -1;
    int loaded = sceSysmoduleLoadModule(id);
    /* The native title imports system modules through its own loader, which
     * resolves their entries before main. General dlsym is unavailable here. */
    if (!p->init || !p->open || !p->read) { p->read = NULL; return; }
    int init = p->init();
    unsigned opened = 0;
    for (unsigned i = 0; i < 8; i++) { p->handles[i] = p->open(user, 0, i, NULL); if (p->handles[i] >= 0) opened++; }
    fprintf(stderr, "[WoWPS5 input] %s module=%#x init=%#x open handles=%u\n", prefix, loaded, init, opened);
}
static void *devices_thread(void *unused)
{
    (void)unused;
    uint64_t report_at = milliseconds() + 10000;
    int diagnostic = !!getenv("WOWPS5_INPUT_DIAGNOSTICS");
    for (;;)
    {
        pthread_mutex_lock(&devices_lock);
        if (pad_handle >= 0)
        {
            struct pad_data samples[64]; int n = scePadRead(pad_handle, samples, 64);
            for (int i = 0; i < n && i < 64; i++) pad_sample(&samples[i]);
            if (n < 0) { struct pad_data neutral = {0}; pad_sample(&neutral); }
        }
        for (unsigned device = 0; device < 8; device++)
        {
            if (keyboard.read && keyboard.handles[device] >= 0)
            {
                struct keyboard_data samples[16]; int n = keyboard.read(keyboard.handles[device], samples, 16);
                for (int i = 0; i < n && i < 16; i++) keyboard_sample(device, &samples[i]);
                if (n < 0) { struct keyboard_data neutral = {0}; keyboard_sample(device, &neutral); }
            }
            if (mouse.read && mouse.handles[device] >= 0)
            {
                struct mouse_data samples[64]; int n = mouse.read(mouse.handles[device], samples, 64);
                for (int i = 0; i < n && i < 64; i++) mouse_sample(device, &samples[i]);
                if (n < 0) { struct mouse_data neutral = {0}; mouse_sample(device, &neutral); }
            }
        }
        if (repeat_key && milliseconds() >= repeat_at)
        { wowps5_input_queue(1, repeat_key, key_flags(repeat_key), 0, 0); repeat_at = milliseconds() + 33; }
        if (diagnostic && milliseconds() >= report_at)
        {
            fprintf(stderr, "[WoWPS5 input] device counts: pad=%llu touch-moves=%llu key-edges=%llu mouse-moves=%llu connected=%d\n",
                (unsigned long long)pad_records, (unsigned long long)touch_moves,
                (unsigned long long)keyboard_edges, (unsigned long long)mouse_moves, pad_connected);
            report_at = milliseconds() + 10000;
        }
        pthread_mutex_unlock(&devices_lock);
        osk_tick();   /* on-screen keyboard: the dialog and the typing, outside the lock */
        usleep(4000);
    }
    return NULL;
}
void wowps5_gamepad_enable(int enabled)
{
    pthread_mutex_lock(&devices_lock); pad_enabled = !!enabled;
    if (!enabled && pad_handle >= 0) { uint8_t zero[2] = {0}; scePadSetVibration(pad_handle, zero); }
    pthread_mutex_unlock(&devices_lock);
}
int wowps5_gamepad_vibrate(unsigned index, unsigned left, unsigned right)
{
    pthread_mutex_lock(&devices_lock);
    int result = index == 0 && pad_connected ? 0 : 1167;
    if (!result && pad_enabled) { uint8_t motors[2] = {left >> 8, right >> 8}; if (scePadSetVibration(pad_handle, motors) < 0) result = 50; }
    pthread_mutex_unlock(&devices_lock);
    return result;
}
void wowps5_devices_start(void)
{
    int32_t user = -1;
    sceUserServiceInitialize(NULL);
    if (sceUserServiceGetInitialUser(&user) < 0) { fprintf(stderr, "[WoWPS5 input] no signed-in console user\n"); return; }
    int init = scePadInit();
    for (int i = 0; i < 10 && pad_handle < 0; i++) { pad_handle = scePadOpen(user, 0, 0, NULL); if (pad_handle < 0) usleep(100000); }
    fprintf(stderr, "[WoWPS5 input] pad init=%#x handle=%#x\n", init, pad_handle);
    if (pad_handle >= 0) scePadSetVibrationMode(pad_handle, 2);
    keyboard.init = keyboard_init; keyboard.open = keyboard_open; keyboard.read = keyboard_read;
    mouse.init = mouse_init; mouse.open = mouse_open; mouse.read = mouse_read;
    peripheral_open(&keyboard, 0x0106, "sceKeyboard", user);
    peripheral_open(&mouse, 0x00a9, "sceMouse", user);
    osk_start(user);   /* on-screen keyboard: settings, sysmodule 0x0096 */
    pthread_t thread;
    if (!pthread_create(&thread, NULL, devices_thread, NULL)) pthread_detach(thread);
    else fprintf(stderr, "[WoWPS5 input] device thread unavailable\n");
}
#endif
