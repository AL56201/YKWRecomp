"""Find code in an NSO that loads given globals (ADRP + LDR/ADD pairs). Debug helper.

Usage: python xref_globals.py <nso> <offset>[=name] ...
Prints each referencing instruction with its enclosing function (nearest preceding eh_frame / symbol start).
"""
import bisect
import struct
import sys

from analyze import eh_frame_functions
from nso import Nso


def main():
    n = Nso(sys.argv[1])
    want = {}
    for a in sys.argv[2:]:
        off, _, name = a.partition('=')
        want[int(off, 16)] = name or off
    # GOT slots holding a wanted address (R_AARCH64_RELATIVE addend, or ABS64/GLOB_DAT to a symbol there)
    for off, typ, sym, add in n.relocs:
        val = add if typ == 1027 else (n.syms[sym][3] + add if typ in (257, 1025) and sym < len(n.syms) else None)
        if val in want and off not in want:
            want[off] = want[val] + '@got'
    starts = sorted(set(eh_frame_functions(n)))
    t0, tlen = n.text
    words = struct.unpack_from(f'<{tlen // 4}I', n.image, t0)
    adrp = {}
    for i, w in enumerate(words):
        pc = t0 + i * 4
        if (w & 0x9F000000) == 0x90000000:
            imm = ((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            adrp[w & 31] = ((pc & ~0xFFF) + (imm << 12), i)
            continue
        rn = (w >> 5) & 31
        if rn not in adrp or i - adrp[rn][1] > 16:
            continue
        page = adrp[rn][0]
        if (w & 0xFFC00000) == 0xF9400000:  # LDR x, [xn, #imm*8]
            tgt = page + ((w >> 10) & 0xFFF) * 8
        elif (w & 0xFFC00000) in (0xBD400000, 0xB9400000, 0xBD000000, 0xB9000000):  # LDR/STR s / w, [xn, #imm*4]
            tgt = page + ((w >> 10) & 0xFFF) * 4
        elif (w & 0xFFC00000) == 0x91000000:  # ADD x, xn, #imm
            tgt = page + ((w >> 10) & 0xFFF)
        else:
            continue
        if tgt in want:
            k = bisect.bisect_right(starts, pc) - 1
            fn = starts[k] if k >= 0 else 0
            print(f'{want[tgt]:24} ref at {pc:#x}  in fn {fn:#x}+{pc - fn:#x}')


if __name__ == '__main__':
    main()
