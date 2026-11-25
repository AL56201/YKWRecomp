"""List the virtual functions of the classes whose RTTI name contains a string. Debug helper.

Usage: python vtable_of.py <nso> <name substring>
Prints each matching class's vtable slots (index, function, size by eh_frame) and a hint for functions that clamp a
float with 1.0 (fmin / fcmp against 1.0 near fcsel).
"""
import bisect
import struct
import sys

from analyze import eh_frame_functions
from nso import Nso


def main():
    n = Nso(sys.argv[1])
    want = sys.argv[2]
    img = n.image
    starts = sorted(set(eh_frame_functions(n)))
    ptr = {}
    for off, typ, sym, add in n.relocs:
        if typ == 1027:
            ptr[off] = add
    t0, tl = n.text

    def cstr(a):
        return img[a:img.index(b'\0', a)].decode('latin1')

    def size_of(f):
        k = bisect.bisect_right(starts, f)
        return (starts[k] - f) if k < len(starts) else 0

    for slot, ti in sorted(ptr.items()):
        name_ptr = ptr.get(ti + 8)
        if name_ptr is None:
            continue
        try:
            name = cstr(name_ptr)
        except ValueError:
            continue
        if want not in name:
            continue
        otop = struct.unpack_from('<q', img, slot - 8)[0]
        if otop != 0:
            continue
        fns = []
        s = slot + 8
        while s in ptr and t0 <= ptr[s] < t0 + tl:
            fns.append(ptr[s])
            s += 8
        if len(fns) < 3:
            continue
        print(f'== {name}  vtable at {slot - 8:#x}, {len(fns)} slots')
        for i, f in enumerate(fns):
            words = struct.unpack_from(f'<{max(size_of(f), 4) // 4}I', img, f)
            ones = sum(1 for w in words if w == 0x1E2E1001)  # fmov s1, #1.0 (a common clamp constant)
            fmin = sum(1 for w in words if (w & 0xFFE0FC00) == 0x1E205800)  # fmin s
            print(f'  [{i:2}] {f:#x} size {size_of(f):5}  fmov#1.0:{ones} fmin:{fmin}')


if __name__ == '__main__':
    main()
