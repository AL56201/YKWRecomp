"""For each code site that loads a given field offset, name the C++ class whose virtual function contains it.

Usage: python rtti_sites.py <nso> <field offset hex> [--float]
Prints site, the containing function (eh_frame start), and the classes (from RTTI typeinfo names) of every vtable that
points at that function; for functions no vtable points at, the vtable classes of their callers (one level up).
"""
import bisect
import struct
import sys

from analyze import eh_frame_functions
from nso import Nso


def main():
    n = Nso(sys.argv[1])
    field = int(sys.argv[2], 16)
    want_float = '--float' in sys.argv
    img = n.image
    t0, tl = n.text
    words = struct.unpack_from(f'<{tl // 4}I', img, t0)
    starts = sorted(set(eh_frame_functions(n)))

    def fn_of(a):
        k = bisect.bisect_right(starts, a) - 1
        return starts[k] if k >= 0 else None

    # pointer values in data, from relocations: address of slot -> target
    ptr = {}
    for off, typ, sym, add in n.relocs:
        if typ == 1027:
            ptr[off] = add
        elif typ in (257, 1025) and sym < len(n.syms) and n.syms[sym][2]:
            ptr[off] = n.syms[sym][3] + add
    # vtable slots pointing at each function
    slots_of = {}
    for slot, tgt in ptr.items():
        slots_of.setdefault(tgt, []).append(slot)

    def cstr(a):
        e = img.index(b'\0', a)
        return img[a:e].decode('latin1')

    def class_of_slot(slot):
        # walk back to the vtable header: [offset-to-top (0 or negative), typeinfo ptr, fn0, fn1...]
        for back in range(1, 400):
            s = slot - 8 * back
            ti = ptr.get(s)
            if ti is None:
                continue
            # typeinfo: [vtable for type_info class + 16, name ptr, ...]
            name_ptr = ptr.get(ti + 8)
            if name_ptr is None:
                continue
            try:
                name = cstr(name_ptr)
            except ValueError:
                continue
            if name and (name[0].isdigit() or name.startswith('N')):
                otop = struct.unpack_from('<q', img, s - 8)[0]
                if -0x10000 < otop <= 0:
                    return name
        return None

    def classes_of_fn(f):
        names = set()
        for slot in slots_of.get(f, []):
            c = class_of_slot(slot)
            if c:
                names.add(c)
        return names

    callers = {}
    for i, x in enumerate(words):
        if (x & 0xFC000000) == 0x94000000:
            o = x & 0x3FFFFFF
            if o & 0x2000000:
                o -= 0x4000000
            callers.setdefault(t0 + i * 4 + o * 4, []).append(t0 + i * 4)

    for i, x in enumerate(words):
        if want_float:
            ok = (x & 0xFFC00000) == 0xBD400000  # LDR s, [x, #imm*4]
        else:
            ok = (x & 0xFFC00000) in (0xB9400000, 0xBD400000)
        if not ok or ((x >> 10) & 0xFFF) * 4 != field:
            continue
        a = t0 + i * 4
        f = fn_of(a)
        cls = classes_of_fn(f)
        how = 'vtable'
        if not cls:
            for c in callers.get(f, [])[:6]:
                cls |= classes_of_fn(fn_of(c))
            how = 'caller'
        print(f'{a:#x}  s{x & 31} <- [x{(x >> 5) & 31}]  fn {f:#x}  {how}: {", ".join(sorted(cls)) or "?"}')


if __name__ == '__main__':
    main()
