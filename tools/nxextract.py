#!/usr/bin/env python3
"""Extract exefs/ and romfs/ from the player's own NSP files (base + optional update).

Usage:
  python -I tools/nxextract.py --keys prod.keys [--titlekeys title.keys] \
      --base base.nsp [--update update.nsp] --out extracted

Writes <out>/exefs/{main,main.npdm,rtld,sdk,subsdk*} and <out>/romfs/... The update's
program NCA replaces exefs wholesale and patches the base RomFS through its BKTR tables.
No keys or game data are written anywhere except <out>.
"""
import argparse, bisect, os, struct, sys
from Crypto.Cipher import AES
from Crypto.Util import Counter

MEDIA = 0x200


# ---------------------------------------------------------------- keys
def load_keys(path):
    keys = {}
    if not path or not os.path.exists(path):
        return keys
    with open(path, encoding="utf-8", errors="ignore") as f:
        for line in f:
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            k, v = k.strip().lower(), v.strip()
            try:
                keys[k] = bytes.fromhex(v)
            except ValueError:
                pass
    return keys


# ---------------------------------------------------------------- PFS0
class Pfs0:
    def __init__(self, read, base=0):
        self.read, self.base = read, base
        hdr = read(base, 0x10)
        if hdr[:4] != b"PFS0":
            raise ValueError("not a PFS0")
        n, strsz = struct.unpack_from("<II", hdr, 4)
        ents = read(base + 0x10, n * 0x18)
        strs = read(base + 0x10 + n * 0x18, strsz)
        data0 = base + 0x10 + n * 0x18 + strsz
        self.files = {}
        for i in range(n):
            off, size, noff = struct.unpack_from("<QQI", ents, i * 0x18)
            name = strs[noff:strs.index(b"\0", noff)].decode()
            self.files[name] = (data0 + off, size)


def file_reader(path):
    f = open(path, "rb")

    def read(off, size):
        f.seek(off)
        return f.read(size)
    return read


# ---------------------------------------------------------------- AES-XTS (Nintendo tweak)
def _gf_mul_x(t):
    v = int.from_bytes(t, "little")
    v = ((v << 1) ^ (0x87 if v >> 127 else 0)) & ((1 << 128) - 1)
    return v.to_bytes(16, "little")


def xts_decrypt(key, data, sector0, sector_size=MEDIA):
    k1, k2 = AES.new(key[:16], AES.MODE_ECB), AES.new(key[16:], AES.MODE_ECB)
    out = bytearray()
    for s in range(len(data) // sector_size):
        t = k2.encrypt((sector0 + s).to_bytes(16, "big"))
        sec = data[s * sector_size:(s + 1) * sector_size]
        for b in range(0, sector_size, 16):
            x = bytes(a ^ c for a, c in zip(sec[b:b + 16], t))
            out += bytes(a ^ c for a, c in zip(k1.decrypt(x), t))
            t = _gf_mul_x(t)
    return bytes(out)


# ---------------------------------------------------------------- NCA
class Section:
    """A decrypted view of one NCA section (offsets relative to the section start)."""

    def __init__(self, nca, idx):
        self.nca, self.idx = nca, idx
        fh = nca.fs_headers[idx]
        self.fh = fh
        self.start, self.end = nca.sections[idx]
        self.size = self.end - self.start
        self.fs_type, self.hash_type, self.enc_type = fh[2], fh[3], fh[4]
        upper = fh[0x140:0x148]
        self.ctr_hi = bytes(reversed(upper))  # 8 bytes, big-endian upper IV
        self.key = nca.ctr_key

    def _ctr(self, off, hi=None):
        hi = hi or self.ctr_hi
        return AES.new(self.key, AES.MODE_CTR,
                       counter=Counter.new(64, prefix=hi, initial_value=(self.start + off) >> 4))

    def read_raw(self, off, size, hi=None):
        """Decrypt [off, off+size) of the section with CTR (optionally another upper IV)."""
        a = off & ~0xF
        pad = off - a
        raw = self.nca.read(self.start + a, size + pad)
        if self.enc_type == 1:
            data = raw
        else:
            data = self._ctr(a, hi).decrypt(raw)
        return data[pad:pad + size]

    read = read_raw

    def data_region(self):
        """(offset, size) of the payload (PFS0 or RomFS level) inside the section."""
        h = self.fh[0x8:0x100]
        if self.hash_type == 2:  # hierarchical sha256
            nlayers = struct.unpack_from("<I", h, 0x24)[0]
            off, size = struct.unpack_from("<QQ", h, 0x28 + (nlayers - 1) * 0x10)
            return off, size
        if self.hash_type == 3:  # IVFC
            assert h[:4] == b"IVFC", h[:4]
            levels = struct.unpack_from("<I", h, 0xC)[0]
            off, size = struct.unpack_from("<QQ", h, 0x10 + (levels - 2) * 0x18)
            return off, size
        raise ValueError("unknown hash type %d" % self.hash_type)


class Nca:
    def __init__(self, read, base, size, keys, titlekey=None):
        self.raw_read, self.base, self.size = read, base, size
        hk = keys["header_key"]
        hdr = xts_decrypt(hk, read(base, 0xC00), 0)
        magic = hdr[0x200:0x204]
        if magic not in (b"NCA3",):
            raise ValueError("unsupported NCA magic %r" % magic)
        self.hdr = hdr
        self.content_type = hdr[0x205]
        self.kaek_index = hdr[0x207]
        self.title_id = struct.unpack_from("<Q", hdr, 0x210)[0]
        gen = max(hdr[0x206], hdr[0x220])
        self.mkrev = gen - 1 if gen > 0 else 0
        self.rights_id = hdr[0x230:0x240]
        self.sections, self.fs_headers = [], []
        for i in range(4):
            s, e = struct.unpack_from("<II", hdr, 0x240 + i * 0x10)
            self.sections.append((s * MEDIA, e * MEDIA))
            self.fs_headers.append(hdr[0x400 + i * 0x200:0x600 + i * 0x200])
        if any(self.rights_id):
            if titlekey is None:
                raise KeyError("missing title key for rights id " + self.rights_id.hex())
            kek = keys["titlekek_%02x" % self.mkrev]
            self.ctr_key = AES.new(kek, AES.MODE_ECB).decrypt(titlekey)
        else:
            names = ["application", "ocean", "system"]
            kak = keys["key_area_key_%s_%02x" % (names[self.kaek_index], self.mkrev)]
            area = AES.new(kak, AES.MODE_ECB).decrypt(hdr[0x300:0x340])
            self.ctr_key = area[0x20:0x30]

    def read(self, off, size):
        return self.raw_read(self.base + off, size)

    def section(self, i):
        return Section(self, i) if self.sections[i][1] else None


# ---------------------------------------------------------------- BKTR (update RomFS)
class PatchedSection:
    """Virtual view of an update's RomFS section, combining base data and patch data."""

    def __init__(self, upd: Section, base: Section):
        self.upd, self.base = upd, base
        fh = upd.fh
        rel_off, rel_size = struct.unpack_from("<QQ", fh, 0x100)
        sub_off, sub_size = struct.unpack_from("<QQ", fh, 0x120)
        rel = upd.read_raw(rel_off, rel_size)
        sub = upd.read_raw(sub_off, sub_size)
        self.rel = self._parse(rel, 0x14, "<QQI")   # (virt, phys, is_patch)
        self.sub = self._parse(sub, 0x10, "<QII")   # (phys, pad, ctr)
        self.virt_size = struct.unpack_from("<Q", rel, 8)[0]
        self.sub_end = sub_off  # patch data ends where the tables start
        self.rel_keys = [e[0] for e in self.rel]
        self.sub_keys = [e[0] for e in self.sub]
        self.fh = fh
        self.hash_type = upd.hash_type

    @staticmethod
    def _parse(blob, esz, fmt):
        nb = struct.unpack_from("<I", blob, 4)[0]
        out = []
        for b in range(nb):
            bo = 0x4000 + b * 0x4000
            n = struct.unpack_from("<I", blob, bo + 4)[0]
            for i in range(n):
                out.append(struct.unpack_from(fmt, blob, bo + 0x10 + i * esz))
        return out

    def _read_patch(self, phys, size):
        out = bytearray()
        while size:
            i = bisect.bisect_right(self.sub_keys, phys) - 1
            end = self.sub[i + 1][0] if i + 1 < len(self.sub) else self.sub_end
            n = min(size, end - phys)
            ctr = self.sub[i][2]
            hi = self.upd.ctr_hi[:4] + ctr.to_bytes(4, "big")
            out += self.upd.read_raw(phys, n, hi)
            phys += n
            size -= n
        return bytes(out)

    def read(self, off, size):
        out = bytearray()
        while size:
            i = bisect.bisect_right(self.rel_keys, off) - 1
            v, p, is_patch = self.rel[i]
            end = self.rel[i + 1][0] if i + 1 < len(self.rel) else self.virt_size
            n = min(size, end - off)
            phys = p + (off - v)
            out += self._read_patch(phys, n) if is_patch else self.base.read_raw(phys, n)
            off += n
            size -= n
        return bytes(out)

    data_region = Section.data_region


# ---------------------------------------------------------------- RomFS
def extract_romfs(read, base, outdir):
    hdr = read(base, 0x50)
    (hsz, dh_off, dh_sz, dm_off, dm_sz, fh_off, fh_sz, fm_off, fm_sz,
     data_off) = struct.unpack_from("<10Q", hdr)
    dmeta = read(base + dm_off, dm_sz)
    fmeta = read(base + fm_off, fm_sz)
    count = 0

    def walk_dir(doff, path):
        nonlocal count
        parent, sib, child, cfile, h, nlen = struct.unpack_from("<6I", dmeta, doff)
        os.makedirs(path, exist_ok=True)
        f = cfile
        while f != 0xFFFFFFFF:
            fp, fsib, foff, fsize, fhash, fnlen = struct.unpack_from("<IIQQII", fmeta, f)
            name = fmeta[f + 0x20:f + 0x20 + fnlen].decode("utf-8")
            dst = os.path.join(path, name)
            with open(dst, "wb") as o:
                pos, left = base + data_off + foff, fsize
                while left:
                    n = min(left, 8 << 20)
                    o.write(read(pos, n))
                    pos += n
                    left -= n
            count += 1
            if count % 500 == 0:
                print("  romfs: %d files" % count, flush=True)
            f = fsib
        c = child
        while c != 0xFFFFFFFF:
            _, csib, _, _, _, cnlen = struct.unpack_from("<6I", dmeta, c)
            cname = dmeta[c + 0x18:c + 0x18 + cnlen].decode("utf-8")
            walk_dir(c, os.path.join(path, cname))
            c = csib

    walk_dir(0, outdir)
    return count


# ---------------------------------------------------------------- NSP
def tickets(nsp, read, keys, extra):
    tk = dict(extra)
    for name, (off, size) in nsp.files.items():
        if name.endswith(".tik"):
            t = read(off, size)
            rid = t[0x2A0:0x2B0].hex()
            tk[rid] = t[0x180:0x190]
    return tk


def load_program_nca(path, keys, tkeys):
    read = file_reader(path)
    nsp = Pfs0(read)
    tk = tickets(nsp, read, keys, tkeys)
    prog = None
    for name, (off, size) in nsp.files.items():
        if not name.endswith(".nca") or name.endswith(".cnmt.nca"):
            continue
        n = _open_nca(read, off, size, keys, tk)
        print("  %s: type=%d title=%016x size=%d" % (name, n.content_type, n.title_id, size))
        if n.content_type == 0:
            prog = n
    return prog


def _open_nca(read, off, size, keys, tk):
    # read rights id from header first, then look up the matching title key
    hdr = xts_decrypt(keys["header_key"], read(off, 0x400), 0)
    rid = hdr[0x230:0x240]
    titlekey = tk.get(rid.hex()) if any(rid) else None
    return Nca(read, off, size, keys, titlekey)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keys", required=True)
    ap.add_argument("--titlekeys")
    ap.add_argument("--base", required=True)
    ap.add_argument("--update")
    ap.add_argument("--out", required=True)
    ap.add_argument("--no-romfs", action="store_true")
    a = ap.parse_args()
    keys = load_keys(a.keys)
    tkeys = {k: v for k, v in load_keys(a.titlekeys).items() if len(v) == 16}

    print("base:", a.base)
    base = load_program_nca(a.base, keys, tkeys)
    upd = None
    if a.update:
        print("update:", a.update)
        upd = load_program_nca(a.update, keys, tkeys)

    code_nca = upd or base
    exefs_sec = next(code_nca.section(i) for i in range(4)
                     if code_nca.section(i) and code_nca.fs_headers[i][2] == 1)
    eoff, _ = exefs_sec.data_region()
    pfs = Pfs0(exefs_sec.read, eoff)
    exdir = os.path.join(a.out, "exefs")
    os.makedirs(exdir, exist_ok=True)
    for name, (off, size) in pfs.files.items():
        with open(os.path.join(exdir, name), "wb") as o:
            o.write(exefs_sec.read(off, size))
        print("  exefs/%s (%d bytes)" % (name, size))

    if a.no_romfs:
        return
    base_rom = next(base.section(i) for i in range(4)
                    if base.section(i) and base.fs_headers[i][2] == 0)
    rom = base_rom
    if upd:
        usec = next((upd.section(i) for i in range(4)
                     if upd.section(i) and upd.fs_headers[i][2] == 0), None)
        if usec is not None and usec.enc_type == 4:
            rom = PatchedSection(usec, base_rom)
        elif usec is not None:
            rom = usec
    roff, _ = rom.data_region()
    n = extract_romfs(rom.read, roff, os.path.join(a.out, "romfs"))
    print("romfs: %d files" % n)


if __name__ == "__main__":
    main()
