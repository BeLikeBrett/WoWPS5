/* The console's on-screen keyboard for the Windows program. SPDX-License-Identifier: MIT
 *
 * Included by wine_devices.c, of which it is a part: it runs on that file's
 * 4 ms device thread, gates that file's pad records and types through the
 * same queue (wowps5_input_queue) a USB keyboard does.
 *
 *   pad record -> osk_pad()   a controller shortcut asks for the keyboard; the
 *                             shortcut's own buttons never reach the game, and
 *                             while the keyboard is up the game has a connected,
 *                             neutral pad
 *   device thread -> osk_tick()  opens the system dialog, polls it, and types
 *                             the confirmed text a character per tick
 *
 * Nothing here waits: the dialog is polled, the text is fed as the queue takes
 * it. Typed text is never logged; the diagnostics are counts.
 *
 * The system interface is libSceImeDialog. The payload SDK has only its stub
 * (target/lib/libSceImeDialog.so, vendor/PS5_PayloadSDK/sce_stubs/libSceImeDialog.c:
 * the export names) and no header, so the layouts and constants below are the
 * PS4 ones, which two independent open sources agree on:
 *   OpenOrbis-PS4-Toolchain include/orbis/_types/ime_dialog.h (OrbisImeDialogSetting,
 *     OrbisDialogResult, OrbisDialogStatus) and include/orbis/_types/sysmodule.h
 *     (ORBIS_SYSMODULE_IME_DIALOG = 0x0096);
 *   shadPS4 src/core/libraries/ime/ime_common.h (OrbisImeDialogParam, OrbisImeOption,
 *     OrbisImeType, OrbisImeEnterLabel, the 0x80bc.... errors) and ime_dialog.h
 *     (OrbisImeDialogStatus, OrbisImeDialogEndStatus, OrbisImeDialogResult).
 * That this firmware's library keeps them is an assumption until it has run. */

#include <stdatomic.h>

enum { OSK_IDLE, OSK_OPENING, OSK_RUNNING, OSK_SETTLE };
enum { OSK_KEYBOARD, OSK_CHAT, OSK_MODES };
enum { OSK_TEXT_MAX = 255 };              /* a chat line of the game; the dialog allows 2048 */
enum { OSK_PAD_BUTTONS = 0xffff };        /* the pad's digital buttons; bit 20 is the touch-pad click, the mouse's */

static struct
{
    /* settings (osk_start reads them from the environment) */
    uint32_t chord[OSK_MODES], members;
    unsigned chord_ms, tap_ms, chat_delay_ms;
    int unicode, automatic, diagnostic;
    /* shortcut gate */
    uint32_t previous, pending, swallowed, tapping;
    uint64_t pending_since[16], tap_until[16];
    /* dialog */
    int state, mode, escape;
    uint64_t poll_at, quiet_until, report_at;
    unsigned opened, confirmed, dismissed, failed;
} osk = { .chord = {0x2 | 0x4, 0}, .members = 0x2 | 0x4,   /* L3+R3; the chat shortcut is off unless WOWPS5_OSK_CHAT names one */
          .chord_ms = 80, .tap_ms = 80, .chat_delay_ms = 150 };

/* The dialog writes the text here (UTF-16, zero-terminated). Wiped as it is typed. */
static uint16_t osk_text[OSK_TEXT_MAX + 1];
static struct { int active, mode, phase, step, shifted; unsigned index, length, erase; uint64_t wait; } osk_send;
enum { SEND_OPEN_CHAT, SEND_ERASE, SEND_TEXT, SEND_UNSHIFT, SEND_CHAT };
static atomic_int osk_dialog_up, osk_auto_request;

/* What the keyboard shortcut typed last, kept while nothing but that shortcut
 * is used: the shortcut again opens the dialog holding it, and a confirmation
 * replaces it in the game's box (that many Backspaces, then the new text). A
 * pad button the game sees, a touch-pad click or the chat shortcut forgets it:
 * the box may no longer be the same one. Never logged. */
static uint16_t osk_last[OSK_TEXT_MAX + 1];
static unsigned osk_last_length;

static void osk_wipe(void)
{
    volatile uint16_t *p = osk_text;
    for (unsigned i = 0; i <= OSK_TEXT_MAX; i++) p[i] = 0;
}
static void osk_forget(void)
{
    volatile uint16_t *p = osk_last;
    if (!osk_last_length) return;
    for (unsigned i = 0; i <= OSK_TEXT_MAX; i++) p[i] = 0;
    osk_last_length = 0;
}
/* The dialog starts from what its buffer holds. */
static void osk_prefill(int mode)
{
    osk_wipe();
    if (mode == OSK_KEYBOARD) for (unsigned i = 0; i < osk_last_length; i++) osk_text[i] = osk_last[i];
}

/* ---- the system dialog ---- */
#ifdef WOWPS5_INPUT_TEST
/* The host test's clock and dialog (tools/native-osk-test.c). */
static uint64_t osk_test_now;
#define OSK_NOW() osk_test_now
static struct { int open_result, poll_result, mode; unsigned opens, closes, aborts; } osk_fake;
static int osk_dialog_open(int mode) { osk_prefill(mode); osk_fake.opens++; osk_fake.mode = mode; return osk_fake.open_result; }
static int osk_dialog_poll(void) { return osk_fake.poll_result; }
static void osk_dialog_close(void) { osk_fake.closes++; }
static void osk_dialog_abort(void) { osk_fake.aborts++; }
#else
#define OSK_NOW() milliseconds()
struct ime_dialog_param                   /* OrbisImeDialogSetting / OrbisImeDialogParam */
{
    int32_t user_id;
    uint32_t type;                        /* 0 default, 1 basic Latin, 2 URL, 3 mail, 4 number */
    uint64_t supported_languages;         /* 0: the user's */
    uint32_t enter_label;                 /* 0 default, 1 send, 2 search, 3 go */
    uint32_t input_method;                /* 0 */
    void *filter;
    uint32_t option;                      /* 0x2 no auto-capitalization, 0x4 password, 0x20 no learning */
    uint32_t max_text_length;             /* <= 2048 */
    uint16_t *input_text_buffer;          /* max_text_length + 1 units: the initial text, then the result */
    float x, y;                           /* in 1920x1080 */
    uint32_t horizontal_alignment, vertical_alignment;   /* 0 left/top, 1 center, 2 right/bottom */
    const uint16_t *placeholder, *title;
    int8_t reserved[16];
};
_Static_assert(sizeof(struct ime_dialog_param) == 96 && offsetof(struct ime_dialog_param, input_text_buffer) == 40 &&
               offsetof(struct ime_dialog_param, placeholder) == 64, "IME dialog ABI");
int sceSysmoduleLoadModule(uint16_t);
int sceImeDialogInit(const void *param, const void *extended);
int sceImeDialogGetStatus(void);          /* 0 none, 1 running, 2 finished */
int sceImeDialogGetResult(void *result);  /* int32 end status: 0 ok, 1 cancelled by the user, 2 aborted; 12 reserved bytes */
int sceImeDialogTerm(void);
int sceImeDialogAbort(void);
static int32_t osk_user;

static int osk_module_load(void) { return sceSysmoduleLoadModule(0x0096); }
/* Ordinary calls only: the address of an imported function can be null in the
 * converted executable where the call itself resolves (see wine_devices.c). */
static int osk_dialog_open(int mode)
{
    /* Room beyond both structures, zeroed, should this firmware's be longer. */
    static union { struct ime_dialog_param param; uint8_t room[256]; } u;
    memset(&u, 0, sizeof(u));
    osk_prefill(mode);
    u.param.user_id = osk_user;
    u.param.max_text_length = OSK_TEXT_MAX;
    u.param.input_text_buffer = osk_text;
    /* A login field must not be capitalized or remembered by the dictionary. */
    u.param.option = mode == OSK_CHAT ? 0 : 0x2 | 0x20;
    u.param.enter_label = mode == OSK_CHAT ? 1 : 0;
    u.param.x = 960; u.param.y = 540;
    u.param.horizontal_alignment = u.param.vertical_alignment = 1;
    int result = sceImeDialogInit(&u.param, NULL);
    if (result >= 0) return result;
    /* Again with what PS4 homebrew is known to pass: user, length and buffer. */
    fprintf(stderr, "[WoWPS5 input] on-screen keyboard: sceImeDialogInit=%#x, trying the plain form\n", result);
    memset(&u, 0, sizeof(u));
    u.param.user_id = osk_user;
    u.param.max_text_length = OSK_TEXT_MAX;
    u.param.input_text_buffer = osk_text;
    return sceImeDialogInit(&u.param, NULL);
}
/* 0: still up. 1: confirmed, the text is in osk_text. 2: cancelled, aborted or gone. */
static int osk_dialog_poll(void)
{
    union { int32_t end; uint8_t room[64]; } result;
    int status = sceImeDialogGetStatus();
    if (status == 1) return 0;
    memset(&result, 0, sizeof(result));      /* its reserved bytes have to be zero: 0x80bc0032 otherwise */
    result.end = -1;
    const int fetched = status == 2 ? sceImeDialogGetResult(&result) : -1;
    unsigned length = 0;
    while (length < OSK_TEXT_MAX && osk_text[length]) length++;
    /* how the dialog ended and how much it left, never what */
    fprintf(stderr, "[WoWPS5 input] on-screen keyboard closed: status=%d result=%#x end=%d length=%u\n", status, fetched, result.end, length);
    /* Confirmed is "finished, and not cancelled or aborted": the end status of a
     * confirmation was assumed to be 0, and text left in the buffer says as much. */
    return status == 2 && fetched >= 0 && (result.end == 0 || (result.end != 1 && result.end != 2 && length)) ? 1 : 2;
}
static void osk_dialog_close(void) { sceImeDialogTerm(); }
static void osk_dialog_abort(void) { sceImeDialogAbort(); }
#endif

/* ---- typing ---- */
/* The key of a character in the layout Wine uses here (the null driver has no
 * layout of its own, so win32u's kbdus_tables: dlls/win32u/input.c): virtual
 * key in the low byte, 0x100 with Shift. 0: no key, the character is sent as
 * itself (KEYEVENTF_UNICODE, which Wine turns into WM_CHAR). */
static unsigned osk_char_key(unsigned ch)
{
    static const char plain[] = "`-=[]\\;',./", shifted[] = "~_+{}|:\"<>?", digits[] = ")!@#$%^&*(";
    static const uint8_t keys[] = {0xc0,0xbd,0xbb,0xdb,0xdd,0xdc,0xba,0xde,0xbc,0xbe,0xbf};
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 'A';
    if (ch >= 'A' && ch <= 'Z') return ch | 0x100;
    if ((ch >= '0' && ch <= '9') || ch == ' ') return ch;
    if (ch < 0x21 || ch > 0x7e) return 0;
    for (unsigned i = 0; i < 10; i++) if (ch == (unsigned char)digits[i]) return ('0' + i) | 0x100;
    for (unsigned i = 0; i < sizeof(keys); i++)
    {
        if (ch == (unsigned char)plain[i]) return keys[i];
        if (ch == (unsigned char)shifted[i]) return keys[i] | 0x100;
    }
    return 0;
}
/* One event of a press and release per call that the queue accepts; nonzero
 * once the release is in. A full queue is tried again on the next tick. */
static int osk_press(unsigned code, unsigned flags)
{
    if (!osk_send.step) { if (!wowps5_input_queue(1, code, flags, 0, 0)) return 0; osk_send.step = 1; }
    if (!wowps5_input_queue(1, code, flags | 2, 0, 0)) return 0;
    osk_send.step = 0;
    return 1;
}
static int osk_shift(int wanted)
{
    if (osk_send.shifted == wanted) return 1;
    if (!wowps5_input_queue(1, 0xa0, wanted ? 0 : 2, 0, 0)) return 0;
    osk_send.shifted = wanted;
    return 1;
}
static void osk_send_start(int mode, uint64_t now)
{
    unsigned length = 0;
    while (length < OSK_TEXT_MAX && osk_text[length]) length++;
    for (unsigned i = length; i <= OSK_TEXT_MAX; i++) osk_text[i] = 0;
    /* the keyboard's text replaces the one it typed before, and is kept in its turn */
    osk_send.erase = mode == OSK_KEYBOARD ? osk_last_length : 0;
    osk_forget();
    if (mode == OSK_KEYBOARD) { for (unsigned i = 0; i < length; i++) osk_last[i] = osk_text[i]; osk_last_length = length; }
    if (!length && !osk_send.erase) return;
    osk_send.active = 1; osk_send.mode = mode; osk_send.index = 0; osk_send.length = length;
    osk_send.step = 0; osk_send.shifted = 0; osk_send.wait = now;
    osk_send.phase = mode == OSK_CHAT ? SEND_OPEN_CHAT : SEND_ERASE;
}
/* Chat: Enter (the game opens its chat box), the text, Enter. A character a tick. */
static void osk_send_tick(uint64_t now)
{
    while (osk_send.active && now >= osk_send.wait) switch (osk_send.phase)
    {
    case SEND_OPEN_CHAT:
        if (!osk_press(0x0d, 0)) return;
        osk_send.phase = SEND_TEXT; osk_send.wait = now + osk.chat_delay_ms;
        break;
    case SEND_ERASE:
        if (!osk_send.erase) { osk_send.phase = SEND_TEXT; break; }
        if (!osk_press(0x08, 0)) return;
        osk_send.erase--;
        return;
    case SEND_TEXT:
    {
        if (osk_send.index == osk_send.length) { osk_send.phase = SEND_UNSHIFT; break; }
        unsigned ch = osk_text[osk_send.index], key = osk.unicode ? 0 : osk_char_key(ch);
        if (ch < 0x20 || ch == 0x7f) { osk_text[osk_send.index++] = 0; break; }   /* no control characters */
        if (!osk_shift(key >> 8)) return;
        if (!(key ? osk_press(key & 0xff, 0) : osk_press(ch, 4))) return;
        osk_text[osk_send.index++] = 0;
        return;
    }
    case SEND_UNSHIFT:
        if (!osk_shift(0)) return;
        if (osk_send.mode != OSK_CHAT) { osk_send.active = 0; osk.quiet_until = now + 1000; return; }
        osk_send.phase = SEND_CHAT; osk_send.wait = now + 50;
        break;
    case SEND_CHAT:
        if (!osk_press(0x0d, 0)) return;
        osk_send.active = 0; osk.quiet_until = now + 1000;
        return;
    }
}

/* ---- the pad ---- */
/* One pad record, in order. Nonzero: the keyboard owns the pad, and the game
 * is to see it connected and neutral. Zero: *buttons, *l2 and *r2 are what the
 * game may see, which is the record itself unless a shortcut button is held back:
 * a shortcut's button is withheld until the shortcut completes (never shown),
 * the chord time passes (shown from then on) or it is released (shown as a tap). */
static int osk_pad(int usable, uint32_t *buttons, uint8_t *l2, uint8_t *r2)
{
    uint64_t now = OSK_NOW();
    uint32_t raw = usable ? *buttons & OSK_PAD_BUTTONS : 0, out;
    if (osk.state == OSK_SETTLE && usable && !raw && *l2 < 32 && *r2 < 32) osk.state = OSK_IDLE;
    if (osk.state == OSK_IDLE && atomic_exchange(&osk_auto_request, 0) && osk.automatic && usable &&
        !osk_send.active && now >= osk.quiet_until)
    { osk.state = OSK_OPENING; osk.mode = OSK_KEYBOARD; }
    if (osk.state != OSK_IDLE)
    {
        /* A shortcut pressed afresh on a pad the system has not taken: the dialog is not there. */
        for (unsigned mode = 0; mode < OSK_MODES; mode++)
            if (osk.state == OSK_RUNNING && osk.chord[mode] && (raw & osk.chord[mode]) == osk.chord[mode] &&
                (osk.previous & osk.chord[mode]) != osk.chord[mode]) osk.escape = 1;
        osk.previous = raw; osk.pending = osk.swallowed = osk.tapping = 0;
        return 1;
    }
    if (!osk.members) return 0;
    osk.swallowed &= raw;
    for (unsigned i = 0; i < 16; i++)
        if (raw & ~osk.previous & ~osk.swallowed & osk.members & (1u << i))
        { osk.pending |= 1u << i; osk.pending_since[i] = now; }
    osk.previous = raw;
    for (unsigned mode = 0; mode < OSK_MODES; mode++)
    {
        uint32_t chord = osk.chord[mode];
        if (!chord || (raw & chord) != chord || (osk.pending & chord) != chord) continue;
        osk.swallowed |= chord; osk.pending &= ~chord;
        if (osk_send.active) break;       /* still typing the last text */
        osk.state = OSK_OPENING; osk.mode = mode;
        osk.pending = osk.swallowed = osk.tapping = 0;
        return 1;
    }
    for (unsigned i = 0; i < 16; i++)
    {
        uint32_t bit = 1u << i;
        if (osk.pending & bit)
        {
            if (!(raw & bit)) { osk.pending &= ~bit; osk.tapping |= bit; osk.tap_until[i] = now + osk.tap_ms; }
            else if (now - osk.pending_since[i] >= osk.chord_ms) osk.pending &= ~bit;
        }
        if ((osk.tapping & bit) && now >= osk.tap_until[i]) osk.tapping &= ~bit;
    }
    out = (raw & ~osk.swallowed & ~osk.pending) | osk.tapping;
    if (out || (*buttons & (1u << 20))) osk_forget();   /* the game is given something else to do */
    *buttons = (*buttons & ~(uint32_t)OSK_PAD_BUTTONS) | out;
    if ((raw ^ out) & 0x100) *l2 = out & 0x100 ? 255 : 0;
    if ((raw ^ out) & 0x200) *r2 = out & 0x200 ? 255 : 0;
    return 0;
}

/* ---- text-focus signals from the Windows side (work/osk-partner/wine-side.diff) ---- */
enum { FOCUS_KINDS = 12 };
static atomic_uint focus_counts[FOCUS_KINDS], focus_on_focused, focus_off_focused, focus_changes;
static int focus_diagnostic;
/* Called by win32u on any Wine thread. kind: 0/1 an input context associated
 * with / removed from a window (flags: 1 the window has the focus, 2 it changed),
 * 2/3 IME opened/closed, 4 composition window placed, 5..9 caret created,
 * destroyed, shown, hidden, moved, 10 input context created, 11 IME disabled. */
void wowps5_text_focus_event(unsigned kind, unsigned flags)
{
    if (kind >= FOCUS_KINDS) return;
    atomic_fetch_add(&focus_counts[kind], 1);
    atomic_fetch_add(&focus_changes, 1);
    if (kind == 0 && (flags & 1)) atomic_fetch_add(&focus_on_focused, 1);
    if (kind == 1 && (flags & 1)) atomic_fetch_add(&focus_off_focused, 1);
    /* Experimental (WOWPS5_OSK_AUTO): the focused window got its input context back. */
    if (kind == 0 && (flags & 3) == 3) atomic_store(&osk_auto_request, 1);
}

/* ---- the device thread ---- */
/* Called with devices_lock released: a system call that is slow to answer
 * holds up this thread's polling, and no thread of the Windows program. */
static void osk_tick(void)
{
    uint64_t now = OSK_NOW();
    if (osk.state == OSK_OPENING)
    {
        int result = osk_dialog_open(osk.mode);
        if (result < 0)
        {
            osk.failed++; osk.state = OSK_SETTLE;
            fprintf(stderr, "[WoWPS5 input] on-screen keyboard: sceImeDialogInit=%#x\n", result);
        }
        else
        {
            if (!osk.opened++) fprintf(stderr, "[WoWPS5 input] on-screen keyboard: first dialog opened\n");
            atomic_store(&osk_dialog_up, 1);
            osk.state = OSK_RUNNING; osk.escape = 0; osk.poll_at = now + 16;
        }
    }
    else if (osk.state == OSK_RUNNING && now >= osk.poll_at)
    {
        if (osk.escape) { osk.escape = 0; osk_dialog_abort(); }
        int result = osk_dialog_poll();
        osk.poll_at = now + 16;
        if (result)
        {
            osk_dialog_close();
            atomic_store(&osk_dialog_up, 0);
            if (result == 1) { osk.confirmed++; osk_send_start(osk.mode, now); }
            else { osk.dismissed++; osk_wipe(); }
            osk.state = OSK_SETTLE; osk.quiet_until = now + 1000;
        }
    }
    osk_send_tick(now);
    if ((osk.diagnostic || focus_diagnostic) && now >= osk.report_at)
    {
        static unsigned reported;
        unsigned changes = atomic_load(&focus_changes) + osk.opened + osk.confirmed + osk.dismissed + osk.failed;
        osk.report_at = now + 1000;
        if (changes == reported) return;
        reported = changes;
        fprintf(stderr, "[WoWPS5 input] on-screen keyboard counts: opened=%u confirmed=%u dismissed=%u failed=%u\n",
            osk.opened, osk.confirmed, osk.dismissed, osk.failed);
        if (focus_diagnostic)
            fprintf(stderr, "[WoWPS5 input] text-focus counts: imc-on=%u (focused %u) imc-off=%u (focused %u) ime-open=%u ime-close=%u "
                "composition-window=%u caret create=%u destroy=%u show=%u hide=%u move=%u imc-create=%u ime-disable=%u\n",
                atomic_load(&focus_counts[0]), atomic_load(&focus_on_focused), atomic_load(&focus_counts[1]),
                atomic_load(&focus_off_focused), atomic_load(&focus_counts[2]), atomic_load(&focus_counts[3]),
                atomic_load(&focus_counts[4]), atomic_load(&focus_counts[5]), atomic_load(&focus_counts[6]),
                atomic_load(&focus_counts[7]), atomic_load(&focus_counts[8]), atomic_load(&focus_counts[9]),
                atomic_load(&focus_counts[10]), atomic_load(&focus_counts[11]));
    }
}

/* Any thread, on the title's way out: a dialog left up is asked to close. Does not wait. */
void wowps5_osk_shutdown(void)
{
    if (atomic_exchange(&osk_dialog_up, 0)) osk_dialog_abort();
}

/* ---- settings ---- */
/* "L3+R3": pad buttons held together. "off" or "none": no shortcut. */
static int osk_parse_chord(const char *text, uint32_t *chord)
{
    static const struct { const char *name; uint32_t bit; } names[] = {
        {"CREATE",1},{"SHARE",1},{"L3",2},{"R3",4},{"OPTIONS",8},{"UP",0x10},{"RIGHT",0x20},{"DOWN",0x40},{"LEFT",0x80},
        {"L2",0x100},{"R2",0x200},{"L1",0x400},{"R1",0x800},{"TRIANGLE",0x1000},{"CIRCLE",0x2000},{"CROSS",0x4000},
        {"SQUARE",0x8000},{"OFF",0},{"NONE",0}};
    uint32_t result = 0;
    while (*text)
    {
        unsigned length = 0, found = 0;
        while (text[length] && text[length] != '+') length++;
        for (unsigned i = 0; i < sizeof(names)/sizeof(*names) && !found; i++)
        {
            unsigned n = 0;
            while (n < length && names[i].name[n] && (text[n] >= 'a' && text[n] <= 'z' ? text[n] - 32 : text[n]) == names[i].name[n]) n++;
            if (n == length && !names[i].name[n]) { result |= names[i].bit; found = 1; }
        }
        if (!found) return 0;
        text += length;
        if (*text == '+') text++;
    }
    *chord = result;
    return 1;
}
static unsigned osk_number(const char *name, unsigned fallback, unsigned low, unsigned high)
{
    const char *text = getenv(name);
    unsigned value = 0;
    if (!text || !*text) return fallback;
    for (; *text >= '0' && *text <= '9' && value < 100000; text++) value = value * 10 + (*text - '0');
    return *text || value < low || value > high ? fallback : value;
}
/* The settings are environment variables: NAME=value lines of /data/wowps5/launch.txt. */
static void osk_settings(void)
{
    static const char *const variables[OSK_MODES] = {"WOWPS5_OSK_KEYBOARD", "WOWPS5_OSK_CHAT"};
    const char *text;
    for (unsigned mode = 0; mode < OSK_MODES; mode++)
        if ((text = getenv(variables[mode])) && !osk_parse_chord(text, &osk.chord[mode]))
            fprintf(stderr, "[WoWPS5 input] on-screen keyboard: %s is not a list of pad buttons; default kept\n", variables[mode]);
    /* A shortcut inside the other would always fire first. */
    if (osk.chord[OSK_KEYBOARD] && (osk.chord[OSK_KEYBOARD] & osk.chord[OSK_CHAT]) == osk.chord[OSK_KEYBOARD])
    {
        fprintf(stderr, "[WoWPS5 input] on-screen keyboard: the chat shortcut contains the keyboard's; chat shortcut off\n");
        osk.chord[OSK_CHAT] = 0;
    }
    else if (osk.chord[OSK_CHAT] && (osk.chord[OSK_KEYBOARD] & osk.chord[OSK_CHAT]) == osk.chord[OSK_CHAT])
    {
        fprintf(stderr, "[WoWPS5 input] on-screen keyboard: the keyboard shortcut contains the chat's; keyboard shortcut off\n");
        osk.chord[OSK_KEYBOARD] = 0;
    }
    osk.members = osk.chord[OSK_KEYBOARD] | osk.chord[OSK_CHAT];
    osk.chord_ms = osk_number("WOWPS5_OSK_CHORD_MS", osk.chord_ms, 20, 500);
    osk.tap_ms = osk.chord_ms < 50 ? 50 : osk.chord_ms;
    osk.chat_delay_ms = osk_number("WOWPS5_OSK_CHAT_DELAY_MS", osk.chat_delay_ms, 0, 5000);
    osk.unicode = (text = getenv("WOWPS5_OSK_TEXT")) && (text[0] | 0x20) == 'u';   /* "unicode"; default "keys" */
    osk.automatic = (text = getenv("WOWPS5_OSK_AUTO")) && text[0] == '1';
    osk.diagnostic = !!getenv("WOWPS5_INPUT_DIAGNOSTICS");
    focus_diagnostic = (text = getenv("WOWPS5_FOCUS_DIAGNOSTICS")) && text[0] == '1';
}
#ifndef WOWPS5_INPUT_TEST
static void osk_start(int32_t user)
{
    osk_user = user;
    osk_settings();
    if (!osk.members && !osk.automatic) { fprintf(stderr, "[WoWPS5 input] on-screen keyboard: off\n"); return; }
    fprintf(stderr, "[WoWPS5 input] on-screen keyboard: module=%#x keyboard=%#x chat=%#x chord=%ums text=%s auto=%d\n",
        osk_module_load(), osk.chord[OSK_KEYBOARD], osk.chord[OSK_CHAT], osk.chord_ms, osk.unicode ? "unicode" : "keys", osk.automatic);
}
#endif
