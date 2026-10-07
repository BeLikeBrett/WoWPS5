/* The on-screen keyboard's logic on the PC: shortcut detection, what the game
 * sees of the pad, the dialog's life, and the typing. The system dialog and the
 * clock are the test's (WOWPS5_INPUT_TEST in app/runtime/wine_osk.c). MIT.
 *
 *   cc -std=c11 -Wall -Wextra -o work/osk-partner/native-osk-test tools/native-osk-test.c -lpthread
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#define WOWPS5_INPUT_TEST
#include "../app/runtime/wine_devices.c"

struct queued { unsigned type, code, flags; int x, y; } captured[4096];
static unsigned queued, refuse_nth, calls, refused;   /* refuse_nth: every nth call finds the queue full */
int wowps5_input_queue(unsigned type, unsigned code, unsigned flags, int x, int y)
{
    if (refuse_nth && !(++calls % refuse_nth)) { refused++; return 0; }
    if (queued >= 4096) abort();
    captured[queued++] = (struct queued){type, code, flags, x, y};
    return 1;
}

static unsigned checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

enum { L3 = 2, R3 = 4, OPTIONS = 8, UP = 0x10, L2 = 0x100, CROSS = 0x4000, TOUCH = 0x100000, TAKEN = 0x80000000u };
enum { X_START = 0x10, X_L3 = 0x40, X_R3 = 0x80, X_A = 0x1000, X_UP = 1 };

static struct pad_data pad = {.connected = 1, .lx = 128, .ly = 128, .rx = 128, .ry = 128};
static unsigned seen;      /* every XInput button the game could have read, ORed */
/* One turn of the device thread, 4 ms later: a pad record, then the keyboard's tick. */
static void turn(uint32_t buttons)
{
    osk_test_now += 4;
    pad.buttons = buttons;
    pad_sample(&pad);
    seen |= gamepad.pad.buttons;
    osk_tick();
}
static void turns(uint32_t buttons, unsigned count) { while (count--) turn(buttons); }
static void fresh(void)
{
    turns(0, 600);         /* everything released, typing finished, quiet time over */
    osk_forget();
    queued = refuse_nth = calls = refused = seen = 0;
    memset(&osk_fake, 0, sizeof(osk_fake));
    pad.lx = pad.ly = pad.rx = pad.ry = 128; pad.l2 = pad.r2 = 0; pad.touches = 0; pad.connected = 1;
}
static void dialog_text(const char *text)
{
    unsigned i = 0;
    for (; text[i]; i++) osk_text[i] = (unsigned char)text[i];
    osk_text[i] = 0;
}
static int text_wiped(void)
{
    for (unsigned i = 0; i <= OSK_TEXT_MAX; i++) if (osk_text[i]) return 0;
    return 1;
}
/* The captured events as text: "+41 -41" key down/up by virtual key, "+u00e9" a character sent as itself. */
static const char *typed(void)
{
    static char line[8192];
    size_t at = 0;
    line[0] = 0;
    for (unsigned i = 0; i < queued; i++)
        at += snprintf(line + at, sizeof(line) - at, "%s%c%s%02x", i ? " " : "", captured[i].flags & 2 ? '-' : '+',
                       captured[i].flags & 4 ? "u" : "", captured[i].code);
    return line;
}

int main(void)
{
    struct gamepad_state state;

    /* ---- settings ---- */
    uint32_t chord = 0;
    CHECK(osk_parse_chord("L3+R3", &chord) && chord == (L3 | R3));
    CHECK(osk_parse_chord("options+l3", &chord) && chord == (OPTIONS | L3));
    CHECK(osk_parse_chord("Cross+Circle+Square+Triangle", &chord) && chord == 0xf000);
    CHECK(osk_parse_chord("CREATE+UP+DOWN+LEFT+RIGHT+L1+R1+L2+R2", &chord) && chord == (1 | 0xf0 | 0xf00));
    CHECK(osk_parse_chord("share", &chord) && chord == 1);
    CHECK(osk_parse_chord("off", &chord) && chord == 0);
    CHECK(osk_parse_chord("none", &chord) && chord == 0);
    CHECK(osk_parse_chord("", &chord) && chord == 0);
    chord = 0x55;
    CHECK(!osk_parse_chord("L3+bogus", &chord) && chord == 0x55);
    CHECK(!osk_parse_chord("L33", &chord) && !osk_parse_chord("L", &chord) && !osk_parse_chord("+L3", &chord));
    /* the defaults, with nothing set */
    osk_settings();
    CHECK(osk.chord[OSK_KEYBOARD] == (L3 | R3) && osk.chord[OSK_CHAT] == 0 && osk.members == (L3 | R3));
    CHECK(osk.chord_ms == 80 && osk.tap_ms == 80 && osk.chat_delay_ms == 150 && !osk.unicode && !osk.automatic && !focus_diagnostic);
    /* a malformed line keeps the default; one shortcut inside the other loses the outer */
    setenv("WOWPS5_OSK_KEYBOARD", "L3+R9", 1); setenv("WOWPS5_OSK_CHAT", "L3+R3+OPTIONS", 1);
    osk_settings();
    CHECK(osk.chord[OSK_KEYBOARD] == (L3 | R3) && osk.chord[OSK_CHAT] == 0 && osk.members == (L3 | R3));
    setenv("WOWPS5_OSK_KEYBOARD", "CREATE", 1); setenv("WOWPS5_OSK_CHAT", "off", 1);
    setenv("WOWPS5_OSK_CHORD_MS", "40", 1); setenv("WOWPS5_OSK_CHAT_DELAY_MS", "300", 1);
    setenv("WOWPS5_OSK_TEXT", "unicode", 1); setenv("WOWPS5_OSK_AUTO", "1", 1); setenv("WOWPS5_FOCUS_DIAGNOSTICS", "1", 1);
    osk_settings();
    CHECK(osk.chord[OSK_KEYBOARD] == 1 && !osk.chord[OSK_CHAT] && osk.members == 1 && osk.chord_ms == 40 && osk.tap_ms == 50);
    CHECK(osk.chat_delay_ms == 300 && osk.unicode && osk.automatic && focus_diagnostic);
    setenv("WOWPS5_OSK_CHORD_MS", "9999", 1); setenv("WOWPS5_OSK_CHAT_DELAY_MS", "abc", 1);
    osk_settings();
    CHECK(osk.chord_ms == 40 && osk.chat_delay_ms == 300);          /* out of range or not a number: kept */
    setenv("WOWPS5_OSK_KEYBOARD", "L3+R3", 1); setenv("WOWPS5_OSK_CHAT", "OPTIONS+L3", 1);
    setenv("WOWPS5_OSK_CHORD_MS", "80", 1); setenv("WOWPS5_OSK_CHAT_DELAY_MS", "150", 1);
    setenv("WOWPS5_OSK_TEXT", "keys", 1); setenv("WOWPS5_OSK_AUTO", "0", 1); setenv("WOWPS5_FOCUS_DIAGNOSTICS", "0", 1);
    osk_settings();
    CHECK(osk.chord[OSK_KEYBOARD] == (L3 | R3) && osk.chord[OSK_CHAT] == (OPTIONS | L3) && osk.chord_ms == 80 && osk.tap_ms == 80);
    CHECK(!osk.unicode && !osk.automatic && !focus_diagnostic);

    /* ---- the keys of the layout ---- */
    CHECK(osk_char_key('a') == 'A' && osk_char_key('z') == 'Z' && osk_char_key('A') == ('A' | 0x100));
    CHECK(osk_char_key('0') == '0' && osk_char_key('7') == '7' && osk_char_key(' ') == ' ');
    CHECK(osk_char_key('!') == ('1' | 0x100) && osk_char_key('@') == ('2' | 0x100) && osk_char_key(')') == ('0' | 0x100));
    CHECK(osk_char_key('/') == 0xbf && osk_char_key('?') == (0xbf | 0x100) && osk_char_key('\\') == 0xdc && osk_char_key('"') == (0xde | 0x100));
    CHECK(!osk_char_key('\n') && !osk_char_key(0x7f) && !osk_char_key(0xe9) && !osk_char_key(0x3042) && !osk_char_key(0));
    {
        unsigned char used[0x200] = {0}; unsigned unique = 0;
        for (unsigned ch = 0x20; ch < 0x7f; ch++)
        { unsigned key = osk_char_key(ch); if (key && key < 0x200 && !used[key]++) unique++; }
        CHECK(unique == 95);       /* every printable ASCII character has a key of its own */
    }

    /* ---- a shortcut button alone ---- */
    fresh();
    turn(CROSS);
    CHECK(gamepad.pad.buttons == X_A);                               /* other buttons are not delayed */
    turns(CROSS | L3, 19);                                          /* 76 ms: L3 is held back */
    CHECK(gamepad.pad.buttons == X_A && !osk.state);
    turns(CROSS | L3, 2);                                           /* past 80 ms: a stick click, late */
    CHECK(gamepad.pad.buttons == (X_A | X_L3));
    turn(CROSS);
    CHECK(gamepad.pad.buttons == X_A);
    /* a quick click is not lost: it reaches the game on release, for 80 ms */
    fresh();
    turns(R3, 5);
    CHECK(!gamepad.pad.buttons);
    turn(0);
    CHECK(gamepad.pad.buttons == X_R3);
    turns(0, 19);
    CHECK(gamepad.pad.buttons == X_R3);
    turns(0, 2);
    CHECK(!gamepad.pad.buttons && !osk_fake.opens);
    /* the second stick after the chord time is two clicks, not the shortcut */
    fresh();
    turns(L3, 30); turns(L3 | R3, 30);
    CHECK(gamepad.pad.buttons == (X_L3 | X_R3) && !osk.state && !osk_fake.opens);
    turns(0, 1);
    CHECK(!gamepad.pad.buttons);
    /* the packet number moves only with what the game sees */
    fresh();
    unsigned packet = gamepad.packet;
    turns(L3, 10);
    CHECK(gamepad.packet == packet);

    /* the touch-pad click stays the mouse's while a shortcut button is held back */
    fresh();
    pad.touches = 1; pad.touch[0] = (struct touch){100, 100, 1, {0}};
    turn(TOUCH | L3);
    CHECK(queued == 1 && captured[0].type == 2 && captured[0].flags == 2 && !gamepad.pad.buttons);
    turn(0);

    /* ---- the keyboard shortcut, confirmed ---- */
    fresh();
    pad.lx = 255; pad.r2 = 200;
    turn(L3);
    turn(L3 | R3);
    CHECK(osk.state == OSK_RUNNING && osk_fake.opens == 1 && osk_fake.mode == OSK_KEYBOARD);
    CHECK(!(seen & (X_L3 | X_R3)));                                  /* the shortcut's presses never were stick clicks */
    CHECK(!wowps5_gamepad_state(0, &state) && !state.pad.buttons && !state.pad.lx && !state.pad.rt);   /* connected, neutral */
    turns(L3 | R3, 5); turns(0, 5);
    pad.touches = 1; pad.touch[0] = (struct touch){100, 100, 1, {0}};
    turn(TOUCH); pad.touch[0].x = 300; turn(TOUCH | CROSS);
    CHECK(!queued && !gamepad.pad.buttons && pad_connected);         /* no mouse, no buttons while it is up */
    pad.touches = 0;
    turns(TAKEN | CROSS, 50);                                       /* the system has the pad: still connected for the game */
    CHECK(!wowps5_gamepad_state(0, &state) && !state.pad.buttons && osk.state == OSK_RUNNING && !queued);
    pad.connected = 0; turn(0);
    CHECK(wowps5_gamepad_state(0, &state) == 1167);                 /* really unplugged */
    pad.connected = 1; turn(TAKEN);
    pad.lx = 128; pad.r2 = 0;
    dialog_text("Hi!"); osk_fake.poll_result = 1;
    turns(CROSS, 8);                                                /* confirmed with Cross, which is still down */
    CHECK(osk_fake.closes == 1 && osk.state == OSK_SETTLE && osk.confirmed == 1);
    turns(CROSS, 20);
    CHECK(!strcmp(typed(), "+a0 +48 -48 -a0 +49 -49 +a0 +31 -31 -a0"));
    CHECK(text_wiped() && !osk_send.active);
    CHECK(!(seen & X_A) && osk.state == OSK_SETTLE);                 /* the confirming press is not the game's */
    turn(0);
    CHECK(osk.state == OSK_IDLE);
    turn(CROSS);
    CHECK(gamepad.pad.buttons == X_A);
    for (unsigned i = 0; i < queued; i++) CHECK(captured[i].type == 1 && !captured[i].x && !captured[i].y);

    /* one character a tick, in order */
    fresh();
    turn(L3 | R3);
    dialog_text("abc"); osk_fake.poll_result = 1;
    while (!osk_send.active) turn(0);
    CHECK(queued == 2);                                             /* the tick that closed the dialog typed the first */
    turn(0); CHECK(queued == 4);
    turn(0); CHECK(queued == 6 && !strcmp(typed(), "+41 -41 +42 -42 +43 -43"));
    turns(0, 10); CHECK(queued == 6);

    /* ---- cancelled: nothing is typed ---- */
    fresh();
    turn(R3); turn(L3 | R3);
    dialog_text("secret"); osk_fake.poll_result = 2;
    turns(0, 40);
    CHECK(!queued && text_wiped() && osk.state == OSK_IDLE && osk_fake.closes == 1 && osk.dismissed == 1);
    /* confirmed empty: nothing either */
    fresh();
    turn(L3 | R3); osk_fake.poll_result = 1;
    turns(0, 40);
    CHECK(!queued && !osk_send.active && osk.state == OSK_IDLE);

    /* ---- the chat shortcut: Enter, the text, Enter ---- */
    fresh();
    turn(OPTIONS); turn(OPTIONS | L3);
    CHECK(osk.state == OSK_RUNNING && osk_fake.mode == OSK_CHAT && !(seen & (X_START | X_L3)));
    turns(TAKEN, 100);
    CHECK(!queued);                                                 /* nothing reaches the game before the text is confirmed */
    dialog_text("gg :)"); osk_fake.poll_result = 1;
    while (!osk_send.active) turn(0);
    CHECK(!strcmp(typed(), "+0d -0d"));
    turns(0, 36);                                                   /* 144 ms: the game is opening its chat box */
    CHECK(queued == 2);
    turns(0, 7);
    CHECK(!strcmp(typed(), "+0d -0d +47 -47 +47 -47 +20 -20 +a0 +ba -ba +30 -30 -a0"));
    turns(0, 11);                                                   /* a short breath before the line is sent */
    CHECK(queued == 14);
    turns(0, 3);
    CHECK(!strcmp(typed(), "+0d -0d +47 -47 +47 -47 +20 -20 +a0 +ba -ba +30 -30 -a0 +0d -0d") && !osk_send.active && text_wiped());
    /* chat cancelled: no Enter either */
    fresh();
    turn(L3 | OPTIONS); osk_fake.poll_result = 2;
    turns(0, 100);
    CHECK(!queued && osk_fake.opens == 1 && osk_fake.mode == OSK_CHAT && osk.state == OSK_IDLE);
    /* a shortcut while the last text is still being typed is swallowed, and opens nothing */
    fresh();
    turn(L3 | OPTIONS);
    dialog_text("0123456789012345678901234567890123456789"); osk_fake.poll_result = 1;
    while (!osk_send.active) turn(0);
    osk_fake.poll_result = 0;
    turns(0, 3); turns(L3 | R3, 30);
    CHECK(osk_fake.opens == 1 && !(seen & (X_L3 | X_R3)) && osk_send.active);
    turns(0, 200);
    CHECK(!osk_send.active && queued == 2 + 80 + 2);

    /* ---- characters with no key go as themselves; forced for all by WOWPS5_OSK_TEXT=unicode ---- */
    fresh();
    turn(L3 | R3);
    osk_text[0] = 'A'; osk_text[1] = 0xe9; osk_text[2] = '\n'; osk_text[3] = 0xd83d; osk_text[4] = 0xde00; osk_text[5] = 'b'; osk_text[6] = 0;
    osk_fake.poll_result = 1;
    turns(0, 40);
    CHECK(!strcmp(typed(), "+a0 +41 -41 -a0 +ue9 -ue9 +ud83d -ud83d +ude00 -ude00 +42 -42"));
    /* ---- the shortcut again: the dialog holds what it typed, a confirmation replaces it ---- */
    fresh();
    turn(L3 | R3); dialog_text("ab"); osk_fake.poll_result = 1;
    turns(0, 40);
    CHECK(!strcmp(typed(), "+41 -41 +42 -42") && osk_last_length == 2);
    osk_fake.poll_result = 0; turns(0, 600);
    turn(L3 | R3); turns(L3 | R3, 2);
    CHECK(osk_fake.opens == 2 && osk_text[0] == 'a' && osk_text[1] == 'b' && !osk_text[2]);
    dialog_text("abc"); osk_fake.poll_result = 1;
    turns(0, 40);
    CHECK(!strcmp(typed(), "+41 -41 +42 -42 +08 -08 +08 -08 +41 -41 +42 -42 +43 -43") && osk_last_length == 3);
    /* confirmed empty: the text is only taken away */
    osk_fake.poll_result = 0; turns(0, 600);
    queued = 0;
    turn(L3 | R3); turns(L3 | R3, 2); dialog_text(""); osk_fake.poll_result = 1;
    turns(0, 40);
    CHECK(!strcmp(typed(), "+08 -08 +08 -08 +08 -08") && !osk_last_length);
    /* a button the game saw in between: another box, nothing is taken away */
    fresh();
    turn(L3 | R3); dialog_text("ab"); osk_fake.poll_result = 1;
    turns(0, 40);
    osk_fake.poll_result = 0; turns(0, 600);
    turns(0x4000, 3); turns(0, 3);
    CHECK(!osk_last_length && !osk_last[0]);
    queued = 0;
    turn(L3 | R3); turns(L3 | R3, 2);
    CHECK(text_wiped());
    dialog_text("c"); osk_fake.poll_result = 1;
    turns(0, 40);
    CHECK(!strcmp(typed(), "+43 -43"));
    fresh();
    osk.unicode = 1;
    turn(L3 | R3); dialog_text("aZ"); osk_fake.poll_result = 1;
    turns(0, 40);
    CHECK(!strcmp(typed(), "+u61 -u61 +u5a -u5a"));
    osk.unicode = 0;

    /* ---- a full queue loses and repeats nothing ---- */
    fresh();
    for (unsigned nth = 1; nth <= 3; nth++)
    {
        fresh();
        turn(L3 | OPTIONS); dialog_text("Ab"); osk_fake.poll_result = 1;
        refuse_nth = nth; turns(0, 100);
        CHECK(nth > 1 || (refused > 10 && !queued && osk_send.active));   /* always full: it waits */
        refuse_nth = nth == 1 ? 0 : nth; turns(0, 300);
        CHECK((nth == 1 || refused >= 4) && !osk_send.active && text_wiped());
        CHECK(!strcmp(typed(), "+0d -0d +a0 +41 -41 -a0 +42 -42 +0d -0d"));
    }

    /* ---- the dialog does not open: the pad comes back, nothing is typed ---- */
    fresh();
    osk_fake.open_result = (int)0x80bc0015;
    turn(L3 | R3);
    CHECK(osk.state == OSK_SETTLE && osk.failed == 1 && !osk_fake.closes && !atomic_load(&osk_dialog_up));
    turns(L3 | R3, 10);
    CHECK(osk_fake.opens == 1 && !(seen & (X_L3 | X_R3)));           /* held: not asked again, not clicked */
    turn(0); turn(UP);
    CHECK(osk.state == OSK_IDLE && gamepad.pad.buttons == X_UP && !queued);

    /* ---- status says running but the pad is the title's and the shortcut is pressed afresh: abort ---- */
    fresh();
    turn(L3 | R3); turns(L3 | R3, 20);
    CHECK(!osk_fake.aborts);                                        /* the opening press, still held, is not that */
    turns(0, 5); turns(L3 | R3, 5);
    CHECK(osk_fake.aborts == 1 && osk.state == OSK_RUNNING);
    osk_fake.poll_result = 2; turns(0, 10);
    CHECK(osk.state == OSK_IDLE && !queued);

    /* ---- the title's way out ---- */
    fresh();
    wowps5_osk_shutdown();
    CHECK(!osk_fake.aborts);
    turn(L3 | R3);
    CHECK(atomic_load(&osk_dialog_up) == 1);
    wowps5_osk_shutdown(); wowps5_osk_shutdown();
    CHECK(osk_fake.aborts == 1);
    osk_fake.poll_result = 2; turns(0, 10);

    /* ---- a trigger in a shortcut: its analog value follows its button ---- */
    fresh();
    osk.chord[OSK_CHAT] = L2 | L3; osk.members = L3 | R3 | L2;
    pad.l2 = 255; turns(L2, 10);
    CHECK(!gamepad.pad.lt);
    turns(L2, 12);
    CHECK(gamepad.pad.lt == 255);
    pad.l2 = 0; turns(0, 2);
    osk.chord[OSK_CHAT] = OPTIONS | L3; osk.members = L3 | R3 | OPTIONS;

    /* ---- both shortcuts off: the pad is untouched ---- */
    fresh();
    osk.chord[OSK_KEYBOARD] = osk.chord[OSK_CHAT] = 0; osk.members = 0;
    turn(L3 | R3 | OPTIONS);
    CHECK(gamepad.pad.buttons == (X_L3 | X_R3 | X_START) && !osk_fake.opens && osk.state == OSK_IDLE);
    osk.chord[OSK_KEYBOARD] = L3 | R3; osk.chord[OSK_CHAT] = OPTIONS | L3; osk.members = L3 | R3 | OPTIONS;

    /* ---- text-focus signals: counted; open the keyboard only when asked to (experimental) ---- */
    fresh();
    wowps5_text_focus_event(0, 3); wowps5_text_focus_event(1, 1); wowps5_text_focus_event(9, 0); wowps5_text_focus_event(99, 0);
    CHECK(atomic_load(&focus_counts[0]) == 1 && atomic_load(&focus_on_focused) == 1 && atomic_load(&focus_counts[1]) == 1 &&
          atomic_load(&focus_off_focused) == 1 && atomic_load(&focus_counts[9]) == 1 && atomic_load(&focus_changes) == 3);
    turns(0, 5);
    CHECK(!osk_fake.opens);                                         /* WOWPS5_OSK_AUTO is off */
    osk.automatic = 1;
    wowps5_text_focus_event(0, 1); wowps5_text_focus_event(0, 2); wowps5_text_focus_event(1, 3);
    turns(0, 5);
    CHECK(!osk_fake.opens);                                         /* not focused, or not a change, or context removed */
    wowps5_text_focus_event(0, 3);
    turn(0);
    CHECK(osk_fake.opens == 1 && osk_fake.mode == OSK_KEYBOARD && osk.state == OSK_RUNNING);
    wowps5_text_focus_event(0, 3);                                  /* while it is up, and just after: not again */
    dialog_text("x"); osk_fake.poll_result = 1; turns(0, 20);
    CHECK(osk_fake.opens == 1 && osk.state == OSK_IDLE && queued == 2);
    wowps5_text_focus_event(0, 3); turns(0, 5);
    CHECK(osk_fake.opens == 1);                                     /* within a second of the last text: not again */
    osk_fake.poll_result = 0; turns(0, 300);
    wowps5_text_focus_event(0, 3); turn(0);
    CHECK(osk_fake.opens == 2);
    osk_fake.poll_result = 2; turns(0, 10);
    osk.automatic = 0;

    printf("%s on-screen keyboard: %u checks, %u failed\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
