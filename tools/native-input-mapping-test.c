/* Device transitions that are difficult to reproduce reliably by hand. MIT. */
#include <assert.h>
#define WOWPS5_INPUT_TEST
#include "../app/runtime/wine_devices.c"
struct queued { unsigned type, code, flags; int x, y; } captured[1024];
static unsigned queued;
int wowps5_input_queue(unsigned type, unsigned code, unsigned flags, int x, int y)
{ assert(queued < 1024); captured[queued++] = (struct queued){type,code,flags,x,y}; return 1; }
int main(void)
{
    struct pad_data p = {.connected=1,.lx=128,.ly=128,.rx=128,.ry=128,.touches=1,.touch={{100,100,1,{0}}}};
    pad_sample(&p); assert(!queued);
    p.touch[0].x += 10; p.touch[0].y -= 5; pad_sample(&p);
    assert(queued==1 && captured[0].x==20 && captured[0].y==-10);
    p.buttons=0x100000; pad_sample(&p); assert(captured[1].flags==2);
    p.touch[0].x=1500; pad_sample(&p); assert(touch_click==1); /* drag stays primary */
    p.buttons=0; pad_sample(&p); assert(captured[queued-1].flags==4);
    p.touch[0].id=2; p.touch[0].x=30; unsigned before=queued; pad_sample(&p); assert(queued==before);
    p.touch[0].x=1500; p.buttons=0x100000; pad_sample(&p); assert(touch_click==2);
    p.connected=0; pad_sample(&p); assert(!touch_click && !pad_connected && captured[queued-1].flags==16);
    p.connected=1; p.touches=0; p.buttons=0x4000|0x800|0x10; p.lx=0; p.ly=0; p.r2=255; pad_sample(&p);
    struct gamepad_state state; assert(!wowps5_gamepad_state(0,&state));
    assert(state.pad.buttons==(0x1000|0x200|1) && state.pad.lx==-32768 && state.pad.ly==32767 && state.pad.rt==255);
    assert(wowps5_gamepad_state(1,&state)==1167 && wowps5_gamepad_state(4,&state)==160);
    unsigned packet=state.packet; pad_sample(&p); assert(gamepad.packet==packet);
    struct keyboard_data k={.connected=1,.modifiers=2,.keys={4}};
    queued=0; keyboard_sample(0,&k); assert(queued==2 && captured[0].code==0xa0 && captured[1].code=='A');
    keyboard_sample(1,&k); assert(queued==2); /* one key held on two keyboards */
    k.connected=0; keyboard_sample(0,&k); assert(queued==2);
    keyboard_sample(1,&k); assert(queued==4 && captured[2].flags==2 && captured[3].flags==2);
    k.connected=1; k.keys[0]=4; k.modifiers=0; keyboard_sample(0,&k);
    k.keys[0]=1; before=queued; keyboard_sample(0,&k); assert(queued==before && key_held['A']);
    k.intercepted=1; keyboard_sample(0,&k); assert(!key_held['A'] && !repeat_key);
    struct mouse_data m={.connected=1,.buttons=1,.x=4,.y=-2,.wheel=-1};
    queued=0; mouse_sample(0,&m); assert(queued==3 && captured[0].x==4 && captured[1].flags==2 && (int)captured[2].code==-120);
    m.buttons=0x80000001; mouse_sample(0,&m); assert(queued==4 && captured[3].flags==4);
    puts("PASS touch movement/rebase/drag/clicks, XInput axes/buttons/packets, keyboard chords/multiple devices/rollover/interception, mouse buttons/wheel");
}
