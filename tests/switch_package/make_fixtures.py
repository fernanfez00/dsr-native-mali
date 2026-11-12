#!/usr/bin/env python3
"""Builds small fake Switch packages (NSP, XCI, an update NSP with a BKTR RomFS patch) with random keys, and the
files expected after installing them, for SwitchPackageTest. Usage: make_fixtures.py OUT_DIR"""
import hashlib, os, struct, sys, random
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

rnd = random.Random(1234)
def rbytes(n): return bytes(rnd.getrandbits(8) for _ in range(n))
def ecb_enc(k, d): e = Cipher(algorithms.AES(k), modes.ECB()).encryptor(); return e.update(d) + e.finalize()
def ctr_crypt(k, upper, abs_off, d):
    assert abs_off % 16 == 0
    iv = struct.pack('>QQ', upper, abs_off >> 4)
    e = Cipher(algorithms.AES(k), modes.CTR(iv)).encryptor(); return e.update(d) + e.finalize()
def xts_enc(k32, d):
    out = b''
    for s in range(len(d) // 0x200):
        e = Cipher(algorithms.AES(k32), modes.XTS(s.to_bytes(16, 'big'))).encryptor()
        out += e.update(d[s*0x200:(s+1)*0x200]) + e.finalize()
    return out
def align(n, a): return (n + a - 1) // a * a
def pad(b, a): return b + bytes(align(len(b), a) - len(b))

HEADER_KEY = rbytes(32)
KGEN = 2  # key generation index (header byte 0x220 = 3)
KAEK = rbytes(16)
TITLEKEK = rbytes(16)
NONE = 0xFFFFFFFF
BUILD_SUPPORTED = bytes.fromhex('5a797a3a5da6c4f3468cb7c84e4dde63') + rbytes(16)
TITLE = 0x01004AB00A260000

def pfs0(files):
    names = b''; offs = []
    for n, _ in files: offs.append(len(names)); names += n.encode() + b'\0'
    names = pad(names, 0x20) if len(names) % 0x20 else names
    hdr = b'PFS0' + struct.pack('<III', len(files), len(names), 0)
    data = b''; ents = b''
    for (n, d), no in zip(files, offs):
        ents += struct.pack('<QQII', len(data), len(d), no, 0); data += d
    return hdr + ents + names + data

def hfs0(files):
    names = b''; offs = []
    for n, _ in files: offs.append(len(names)); names += n.encode() + b'\0'
    names = pad(names, 0x20)
    hdr = b'HFS0' + struct.pack('<III', len(files), len(names), 0)
    data = b''; ents = b''
    for (n, d), no in zip(files, offs):
        ents += struct.pack('<QQIIQ', len(data), len(d), no, 0x200, 0) + hashlib.sha256(d[:0x200]).digest()
        data += pad(d, 0x200)
    return hdr + ents + names + data

def nso(build_id):
    h = bytearray(0x100); h[0:4] = b'NSO0'; h[0x40:0x60] = build_id
    return bytes(h) + rbytes(0x300)

def romfs(files):
    """files: dict path -> bytes. Directory and file tables as the format has them (hash tables unused)."""
    dirs = {'': []}
    for p in sorted(files):
        parts = p.split('/')
        for i in range(1, len(parts)):
            dirs.setdefault('/'.join(parts[:i]), [])
    # dir entries: offsets
    dir_list = sorted(dirs, key=lambda d: (d.count('/') if d else -1, d))
    def name(p): return p.split('/')[-1].encode()
    dir_off = {}; o = 0
    for d in dir_list:
        dir_off[d] = o; o += align(0x18 + (len(name(d)) if d else 0), 4)
    file_list = sorted(files); file_off = {}; o = 0
    for f in file_list:
        file_off[f] = o; o += align(0x20 + len(name(f)), 4)
    def parent(p): return p.rsplit('/', 1)[0] if '/' in p else ''
    data = b''; fdata = {}
    for f in file_list:
        data = pad(data, 0x10); fdata[f] = len(data); data += files[f]
    dir_tab = b''
    for d in dir_list:
        kids_d = [x for x in dir_list if x and parent(x) == d]
        kids_f = [x for x in file_list if parent(x) == d]
        sib = NONE
        if d:
            sibs = [x for x in dir_list if x and parent(x) == parent(d)]
            i = sibs.index(d); sib = dir_off[sibs[i + 1]] if i + 1 < len(sibs) else NONE
        n = name(d) if d else b''
        e = struct.pack('<IIIIII', dir_off[parent(d)] if d else 0, sib, dir_off[kids_d[0]] if kids_d else NONE,
                        file_off[kids_f[0]] if kids_f else NONE, NONE, len(n)) + n
        dir_tab += pad(e, 4)
    file_tab = b''
    for f in file_list:
        sibs = [x for x in file_list if parent(x) == parent(f)]
        i = sibs.index(f); sib = file_off[sibs[i + 1]] if i + 1 < len(sibs) else NONE
        n = name(f)
        e = struct.pack('<IIQQII', dir_off[parent(f)], sib, fdata[f], len(files[f]), NONE, len(n)) + n
        file_tab += pad(e, 4)
    dh = struct.pack('<I', NONE) * 4; fh = struct.pack('<I', NONE) * 4
    o = 0x50
    dh_off = o; o += len(dh); dt_off = o; o += len(dir_tab); fh_off = o; o += len(fh); ft_off = o; o += len(file_tab)
    data_off = align(o, 0x10)
    hdr = struct.pack('<QQQQQQQQQQ', 0x50, dh_off, len(dh), dt_off, len(dir_tab), fh_off, len(fh), ft_off,
                      len(file_tab), data_off)
    img = hdr + dh + dir_tab + fh + file_tab
    return pad(img, 0x10) + data

def ivfc_image(level6):
    """A RomFS section image: level-5 hashes of level 6 at 0, level 6 after. Returns (image, ivfc hash data)."""
    bl = 14
    l6 = pad(level6, 1 << bl)
    hashes = b''.join(hashlib.sha256(l6[i:i + (1 << bl)]).digest() for i in range(0, len(l6), 1 << bl))
    l5 = pad(hashes, 1 << bl)
    l6_off = len(l5)
    image = l5 + l6
    levels = [(0, 0x20, bl)] * 4 + [(0, len(hashes), bl), (l6_off, len(level6), bl)]
    hd = b'IVFC' + struct.pack('<III', 0x20000, 0x20, 7)
    for off, size, b in levels: hd += struct.pack('<QQII', off, size, b, 0)
    hd = hd + bytes(0xF8 - len(hd))
    return image, hd

def fs_header(fs_type, hash_type, enc_type, hash_data, gen, secure, patch_info=b''):
    h = bytearray(0x200)
    struct.pack_into('<HBBB', h, 0, 2, fs_type, hash_type, enc_type)
    h[8:8 + len(hash_data)] = hash_data
    h[0x100:0x100 + len(patch_info)] = patch_info
    struct.pack_into('<II', h, 0x140, gen, secure)
    return bytes(h)

def pfs_hash_data(pfs):
    bs = 0x1000
    p = pad(pfs, 0x10)
    table = b''.join(hashlib.sha256(pfs[i:i + bs]).digest() for i in range(0, len(pfs), bs))
    region_ht = pad(table, 0x200)
    pfs_off = len(region_ht)
    image = region_ht + pfs
    hd = hashlib.sha256(table).digest() + struct.pack('<II', bs, 2) + struct.pack('<QQQQ', 0, len(table), pfs_off, len(pfs))
    return image, hd

def make_nca(program_id, sections, key, rights_id=None):
    """sections: list of (fs_header, plain image, encrypt function(abs_off, data) -> data)."""
    body = b''; entries = []; start = 0xC00
    for fsh, image, enc in sections:
        st = start + len(body)
        img = pad(image, 0x200)
        body += enc(st, img)
        entries.append((st, st + len(img)))
    h = bytearray(0xC00)
    h[0x200:0x204] = b'NCA3'; h[0x204] = 0; h[0x205] = 0; h[0x206] = 2; h[0x207] = 0
    struct.pack_into('<QQ', h, 0x208, 0xC00 + len(body), program_id)
    h[0x220] = KGEN + 1
    if rights_id: h[0x230:0x240] = rights_id
    for i, (st, en) in enumerate(entries): struct.pack_into('<II', h, 0x240 + i * 16, st // 0x200, en // 0x200)
    for i, (fsh, _, _) in enumerate(sections):
        h[0x280 + i * 0x20:0x2A0 + i * 0x20] = hashlib.sha256(fsh).digest()
        h[0x400 + i * 0x200:0x600 + i * 0x200] = fsh
    area = bytearray(rbytes(0x40)); area[0x20:0x30] = key
    h[0x300:0x340] = ecb_enc(KAEK, bytes(area))
    return xts_enc(HEADER_KEY, bytes(h)) + body

def ticket(rights_id, title_key):
    t = bytearray(4 + 0x100 + 0x3C + 0x180)
    struct.pack_into('<I', t, 0, 0x10004)
    d = 0x140
    t[d + 0x40:d + 0x50] = ecb_enc(TITLEKEK, title_key)
    t[d + 0x141] = 0; t[d + 0x145] = KGEN
    t[d + 0x160:d + 0x170] = rights_id
    return bytes(t)

def ctr_section(key, gen, secure):
    upper = secure << 32 | gen
    return lambda abs_off, d: ctr_crypt(key, upper, abs_off, d)

def bucket_tree(entries, entry_size, end):
    """BKTR table: L1 node (header + entry-set offsets), then entry-set nodes of 0x4000 bytes."""
    per = (0x4000 - 0x10) // entry_size
    sets = [entries[i:i + per] for i in range(0, len(entries), per)]
    node = struct.pack('<IIQ', 0, len(sets), end)
    for s in sets: node += struct.pack('<Q', struct.unpack_from('<Q', s[0])[0])
    out = pad(node, 0x4000)
    for i, s in enumerate(sets):
        nxt = struct.unpack_from('<Q', sets[i + 1][0])[0] if i + 1 < len(sets) else end
        out += pad(struct.pack('<IIQ', i, len(s), nxt) + b''.join(s), 0x4000)
    hdr = b'BKTR' + struct.pack('<III', 1, len(entries), 0)
    return out, hdr

def main(out):
    os.makedirs(out, exist_ok=True)
    base_files = {
        'dvdbnd0.bhd5': rbytes(5000), 'dvdbnd0.bdt': rbytes(300000), 'dvdbnd1.bhd5': rbytes(800),
        'dvdbnd1.bdt': rbytes(70000), 'sd/menu/a.bin': rbytes(1234), 'sd/menu/b.bin': b'', 'sd/x.txt': b'hello',
    }
    upd_files = dict(base_files)
    b = bytearray(upd_files['dvdbnd0.bdt']); b[100000:100100] = rbytes(100); upd_files['dvdbnd0.bdt'] = bytes(b)
    upd_files['sd/menu/a.bin'] = rbytes(1234)  # same sizes: most of the image maps to the base

    base_key, upd_key, title_key = rbytes(16), rbytes(16), rbytes(16)
    rights = struct.pack('>Q', TITLE) + bytes(7) + bytes([KGEN])
    base_exefs = pfs0([('main', nso(rbytes(32))), ('main.npdm', rbytes(100)), ('sdk', nso(rbytes(32)))])
    upd_exefs = pfs0([('main', nso(BUILD_SUPPORTED)), ('main.npdm', rbytes(100))])

    def base_nca(key, rights_id):
        ex_img, ex_hd = pfs_hash_data(base_exefs)
        rom_img, rom_hd = ivfc_image(romfs(base_files))
        fs0 = fs_header(1, 2, 3, ex_hd, 0, 0)
        fs1 = fs_header(0, 3, 3, rom_hd, 1, 0)
        nca = make_nca(TITLE, [(fs0, ex_img, ctr_section(key, 0, 0)), (fs1, rom_img, ctr_section(key, 1, 0))],
                       bytes(16) if rights_id else key, rights_id)
        # section 1 plain image and start, for the update's relocation
        return nca, rom_img
    base_nca_t, base_rom = base_nca(title_key, rights)  # NSP: title key in the ticket
    base_nca_k, _ = base_nca(base_key, None)  # XCI: key area
    # The key area of a rights-ID NCA is unused; the base image with a title key is encrypted with title_key.

    # Update: the patched RomFS image, built from base chunks and new data.
    v_img, v_hd = ivfc_image(romfs(upd_files))
    v_img = pad(v_img, 0x4000)
    base_chunks = {}
    for o in range(0, len(base_rom) - 0x3FFF, 0x4000): base_chunks.setdefault(base_rom[o:o + 0x4000], o)
    reloc = []; patch_data = b''
    for o in range(0, len(v_img), 0x4000):
        c = v_img[o:o + 0x4000]
        if c in base_chunks: e = (o, base_chunks[c], 0)
        else: e = (o, len(patch_data), 1); patch_data += c
        if reloc and reloc[-1][2] == e[2] and reloc[-1][1] + (o - reloc[-1][0]) == e[1]: continue
        reloc.append(e)
    assert any(e[2] == 0 for e in reloc) and any(e[2] == 1 for e in reloc), reloc
    reloc_entries = [struct.pack('<QQI', v, p, i) for v, p, i in reloc]
    ex_img, ex_hd = pfs_hash_data(upd_exefs)
    ex_size = len(pad(ex_img, 0x200))
    sec1_start = 0xC00 + ex_size
    secure, gens = 0x1234, [5, 9, 13]
    cut = [0, len(patch_data) // 3 // 16 * 16 + 0x30, len(patch_data) * 2 // 3 // 16 * 16]
    enc_patch = b''
    for i, c in enumerate(cut):
        e = cut[i + 1] if i + 1 < len(cut) else len(patch_data)
        enc_patch += ctr_crypt(upd_key, secure << 32 | gens[i], sec1_start + c, patch_data[c:e])
    sub_entries = [struct.pack('<QII', c, 0, gens[i]) for i, c in enumerate(cut)]
    ind_off = align(len(patch_data), 0x4000)
    ind_tab, ind_hdr = bucket_tree(reloc_entries, 0x14, len(v_img))
    sub_off = ind_off + len(ind_tab)
    sub_tab, sub_hdr = bucket_tree(sub_entries, 0x10, ind_off)
    tables_plain = pad(ind_tab, 1) + sub_tab
    patch_info = struct.pack('<QQ', ind_off, len(ind_tab)) + ind_hdr + struct.pack('<QQ', sub_off, len(sub_tab)) + sub_hdr
    sec_gen = 2
    fs1 = fs_header(0, 3, 4, v_hd, sec_gen, secure, patch_info)
    image = enc_patch + bytes(ind_off - len(patch_data)) + tables_plain
    def enc1(abs_off, d):
        assert abs_off == sec1_start
        tab = ctr_crypt(upd_key, secure << 32 | sec_gen, abs_off + ind_off, d[ind_off:])
        return d[:ind_off] + tab
    fs0 = fs_header(1, 2, 3, ex_hd, 0, 0)
    upd_nca = make_nca(TITLE | 0x800, [(fs0, ex_img, ctr_section(upd_key, 0, 0)), (fs1, image, enc1)], upd_key)

    tik = ticket(rights, title_key)
    open(os.path.join(out, 'base.nsp'), 'wb').write(pfs0([('aa.nca', base_nca_t), (rights.hex() + '.tik', tik),
                                                         ('bb.cnmt.nca', rbytes(0xC00))]))
    open(os.path.join(out, 'update.nsp'), 'wb').write(pfs0([('cc.nca', upd_nca)]))
    secure_p = hfs0([('dd.nca', base_nca_k)])
    root = hfs0([('update', hfs0([])), ('normal', hfs0([])), ('secure', secure_p)])
    xci = bytearray(0x200); xci[0x100:0x104] = b'HEAD'; struct.pack_into('<QQ', xci, 0x130, 0x200, 0)
    open(os.path.join(out, 'game.xci'), 'wb').write(bytes(xci) + root)
    with open(os.path.join(out, 'prod.keys'), 'w') as k:
        k.write('header_key = %s\n' % HEADER_KEY.hex())
        k.write('key_area_key_application_%02x = %s\n' % (KGEN, KAEK.hex()))
        k.write('titlekek_%02x = %s\n' % (KGEN, TITLEKEK.hex()))
    for name, files, exefs in (('expect_base', base_files, base_exefs), ('expect_update', upd_files, upd_exefs)):
        for p, d in files.items():
            os.makedirs(os.path.dirname(os.path.join(out, name, p)), exist_ok=True)
            open(os.path.join(out, name, p), 'wb').write(d)
        os.makedirs(os.path.join(out, name, 'exefs'), exist_ok=True)
        import io
        n, = struct.unpack_from('<I', exefs, 4); sl, = struct.unpack_from('<I', exefs, 8)
        data = 16 + n * 0x18 + sl
        for i in range(n):
            o, s, no, _ = struct.unpack_from('<QQII', exefs, 16 + i * 0x18)
            nm = exefs[16 + n * 0x18 + no:].split(b'\0')[0].decode()
            open(os.path.join(out, name, 'exefs', nm), 'wb').write(exefs[data + o:data + o + s])

main(sys.argv[1])
