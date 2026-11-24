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
# The engine paces frames in vsyncs: SetFrameRate(this, interval, base) at main+0x2beee0 and SetInterval(this,
# interval) at main+0x2bf770 store base (logic step, in vsyncs) at +0x7d0, the interval (vsyncs per frame, at most 6)
# at +0x7d4, and interval / base at +0x7dc: how many 60 Hz logic steps one frame stands for, which game code scales
# its movement and timers by. Boot asks for 2 (30 fps). The clamp to 6 is replaced by ykw_frame_interval() (runtime/
# hle/framerate.c), which can lower the interval to the base: the engine then computes the matching time scale itself.
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


# The time scale itself (str s0, [x0, #0x7dc]: interval / base) goes through ykw_time_scale(), which scales it by
# 60 / our vsync rate: with vsync at the display's rate (75, 120, 144 Hz...) the game keeps its 60 Hz speed.
TIME_SCALE_SITES = {MAIN + 0x2bef24: 0xbd07dc00, MAIN + 0x2bf7a4: 0xbd07dc00}  # str s0, [x0, #0x7dc]


def _time_scale(gen, a):
    w = _word(gen, a)
    assert w == TIME_SCALE_SITES[a], f'{a:#x}: {w:#010x}'
    return ('{ extern float ykw_time_scale(float); float s; memcpy(&s, &c->v[0], 4); s = ykw_time_scale(s); '
            f'ST32(c->x[0] + 0x7dc, *(uint32_t *)&s); }}  /* {a:#x}: str s0, [x0, #0x7dc] (time scale) */')


for _a in TIME_SCALE_SITES:
    PATCHES[_a] = _time_scale
