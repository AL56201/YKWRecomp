"""Callers (BL sites) of the function containing each given address. Debug helper.

Usage: python callers.py <nso> <addr> ...   The function start is taken as the instruction after the last
RET / B / BR before addr (good enough for small leaf wrappers).
"""
import struct
import sys

from nso import Nso


def main():
    n = Nso(sys.argv[1])
    t0, tlen = n.text
    words = struct.unpack_from(f'<{tlen // 4}I', n.image, t0)

    def w(a):
        return words[(a - t0) // 4]

    def is_end(x):
        return x == 0xD65F03C0 or x == 0 or (x & 0xFC000000) == 0x14000000 or (x & 0xFFFFFC1F) == 0xD61F0000
    bl = {}
    for i, x in enumerate(words):
        if (x & 0xFC000000) == 0x94000000:
            off = x & 0x3FFFFFF
            if off & 0x2000000:
                off -= 0x4000000
            bl.setdefault(t0 + i * 4 + off * 4, []).append(t0 + i * 4)
    for arg in sys.argv[2:]:
        a = int(arg, 16)
        s = a
        while s > t0 and not is_end(w(s - 4)):
            s -= 4
        sites = bl.get(s, [])
        print(f'{a:#x}: function {s:#x}, {len(sites)} callers: ' + ' '.join(f'{c:#x}' for c in sites[:40]))


if __name__ == '__main__':
    main()
