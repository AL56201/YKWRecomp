"""Code patches and hook sites for the main module (see gen.py).

HOOKS:   {guest address: (expected instruction word, [extra branch targets], comment)}
PATCHES: {guest address: fn(module, address) -> C code replacing that instruction}

Every entry checks the original instruction word, so a wrong build fails loudly.
"""
import struct

MAIN = 0x7000000000
HOOKS = {}
PATCHES = {}


def _word(gen, a):
    return struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]


# ---- frame rate
# The engine's frame pacing (main+0x2bf850, on its logic thread) waits on an event its vsync thread signals each vsync
# (main+0x259af0), then reads the vsync count that thread keeps (main+0x252e00) and, if it hasn't moved since the
# last frame, waits again. But the vsync thread signals the event (main+0x259ae8) before it adds to the count
# (main+0x252e10, called from main+0x2bf978): woken first, the logic thread read the old count and slept a whole
# vsync more - every second or two here (a 20-40 ms hitch), as host threads are scheduled. The count now goes up just
# before the signal, and not after (main+0x252e10 has no other caller). YKW_PACING_RACE=1: the game's own order
# (hle/vi.c ykw_pacing_race).
VSYNC_COUNT_FIRST_SITE = MAIN + 0x259ae0   # mov x0, x19       (before the tail call to SignalLightEvent)
VSYNC_COUNT_AFTER_SITE = MAIN + 0x2bf978   # bl #0x252e10      (the count's increment, after the signal)


def _vsync_count_first(gen, a):
    w = _word(gen, a)
    assert w == 0xaa1303e0, f'{a:#x}: {w:#010x}'
    return ('{ extern int ykw_pacing_race(void); extern void f_7000252e10(Cpu *); '
            'if (!ykw_pacing_race()) { uint64_t lr = c->x[30]; c->x[30] = 0x7000259ae4ull; f_7000252e10(c); '
            'c->x[30] = lr; } c->x[0] = c->x[19]; }'
            f'  /* {a:#x}: mov x0, x19 - the vsync count up first (tools/patches.py) */')


def _vsync_count_after(gen, a):
    w = _word(gen, a)
    assert w == 0x97fe4d26, f'{a:#x}: {w:#010x}'
    return ('{ extern int ykw_pacing_race(void); extern void f_7000252e10(Cpu *); '
            f'if (ykw_pacing_race()) {{ c->x[30] = {a + 4:#x}ull; f_7000252e10(c); }} }}'
            f'  /* {a:#x}: bl #0x252e10 - done before the signal instead (tools/patches.py) */')


PATCHES[VSYNC_COUNT_FIRST_SITE] = _vsync_count_first
PATCHES[VSYNC_COUNT_AFTER_SITE] = _vsync_count_after


# The engine paces frames in vsyncs: SetFrameRate(this, interval, base) at main+0x2beee0 and SetInterval(this,
# interval) at main+0x2bf770 store base (logic step, in vsyncs) at +0x7d0 and the interval (vsyncs per frame, at most
# 6) at +0x7d4. Boot asks for 2 (30 fps). Logic advances a fixed 1/60 s per vsync (the float interval / base it also
# stores, at +0x7dc, is read by no code), so 60 fps (interval 1) is the fastest the game runs at its right speed.
# The clamp to 6 is replaced by ykw_frame_interval() (runtime/hle/framerate.c), which lowers the interval to the base
# when fps = 60.
FRAME_INTERVAL_SITES = {
    # address: (instruction word, register holding the requested interval, register holding base)
    MAIN + 0x2bf788: (0x1a8ab129, 9, 8),    # SetInterval:  csel w9, w9, w10, lt   (w10 = 6)
    MAIN + 0x2bef04: (0x1a89b149, 10, 8),   # SetFrameRate: csel w9, w10, w9, lt   (w9 = 6)
}


def _frame_interval(gen, a):
    want, req, base = FRAME_INTERVAL_SITES[a]
    w = _word(gen, a)
    assert w == want, f'{a:#x}: {w:#010x}, expected {want:#010x}'
    return (f'{{ extern uint32_t ykw_frame_interval(uint32_t, uint32_t); uint32_t r = (uint32_t)c->x[{req}]; '
            f'if ((int32_t)r >= 6) r = 6; c->x[9] = ykw_frame_interval(r, (uint32_t)c->x[{base}]); }}'
            f'  /* {a:#x}: frame interval clamp (tools/patches.py) */')


for _a in FRAME_INTERVAL_SITES:
    PATCHES[_a] = _frame_interval


# (debug) The field camera's update (ywCCamera_Rpg, vtable slot 10, main+0x197070) compares where the camera is
# (+0x2cc) with where it is heading (+0x624) each frame; YKW_CAM_LOG logs both (hle/framerate.c). The target's speed
# is the game's speed: how the frame-rate cap was measured.
CAM_PROBE_SITE = MAIN + 0x1980c8  # ldr s0, [x21, #0x624]


def _cam_probe(gen, a):
    w = _word(gen, a)
    assert w == 0xbd4626a0, f'{a:#x}: {w:#010x}'
    return ('{ extern void ykw_cam_probe(uint64_t, uint32_t); ykw_cam_probe(c->x[21], (uint32_t)c->v[8]); '
            'c->v[0] = LD32(c->x[21] + 0x624); }  /* ' f'{a:#x}' ': ldr s0, [x21, #0x624] (camera probe) */')


PATCHES[CAM_PROBE_SITE] = _cam_probe


# The field camera's follow: the update's argument is the frame's length in vsyncs (2 at 30 fps, 1 at 60); it is
# truncated to a count (fcvtzu w19, s8 at main+0x198898) and the follow runs once per vsync in a loop
# (main+0x198954..0x198988). But the loop first tests the count's lowest bit (tbnz w19, #0) and, for an odd count,
# skips the follow on every pass: at 30 fps the count is always 2, so the camera follows smoothly; at 60 fps it is 1,
# so the camera stands still and only catches up (in one 40-150 px jump) on a slow frame with a count of 2. Without
# the test an odd count follows like an even one: one 1/60 s step per vsync, so the speed is the same at any rate.
CAM_FOLLOW_ODD_SITE = MAIN + 0x198958  # tbnz w19, #0, #0x19896c


def _cam_follow_odd(gen, a):
    w = _word(gen, a)
    assert w == 0x370000b3, f'{a:#x}: {w:#010x}'
    return f'/* {a:#x}: tbnz w19, #0 (odd vsync count: the camera skipped its follow) removed (tools/patches.py) */'


PATCHES[CAM_FOLLOW_ODD_SITE] = _cam_follow_odd
