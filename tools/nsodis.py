"""Disassemble a range of an NSO, naming imported calls. Debug helper.

Usage: python dis.py <nso> <start hex> <end hex>
"""
import sys

import capstone

from nso import Nso


def main():
    n = Nso(sys.argv[1])
    lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    got = {r[0]: r[2] for r in n.relocs if r[1] == 1026}

    def plt_name(a):
        ins = list(md.disasm(bytes(n.image[a:a + 8]), a))
        if len(ins) == 2 and ins[0].mnemonic == 'adrp':
            page = int(ins[0].op_str.split('#')[1], 16)
            off = int(ins[1].op_str.split('#')[-1].rstrip(']'), 16)
            s = got.get(page + off)
            return n.syms[s][0] if s is not None else None
        return None
    for i in md.disasm(bytes(n.image[lo:hi]), lo):
        extra = ''
        if i.mnemonic == 'bl':
            t = int(i.op_str.lstrip('#'), 16)
            extra = '  -> ' + (plt_name(t) or hex(t))
        print(f'{i.address:x}: {i.mnemonic} {i.op_str}{extra}')


if __name__ == '__main__':
    main()
