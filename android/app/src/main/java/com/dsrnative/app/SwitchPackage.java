package com.dsrnative.app;

import java.io.BufferedReader;
import java.io.Closeable;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.Reader;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

import javax.crypto.Cipher;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;

/**
 * Extracts the game from Switch packages (.nsp, .xci), so players do not have to unpack them with other tools:
 * the program's exefs and the RomFS files are written to the game folder, as the loader expects them.
 *
 * Content is encrypted; the keys come from the player's own prod.keys (dumped from their Switch; none are
 * included). Supported: NSP (PFS0) with common tickets, XCI (HFS0, secure partition), an update next to the base
 * game (its RomFS patch, "BKTR": IndirectStorage over the base RomFS + AesCtrEx), checked with the section hashes.
 * Not supported: compressed packages (.nsz, .xcz), personalized tickets, sparse base content.
 *
 * Plain Java (no Android classes): tested on the host (tests/switch_package).
 */
final class SwitchPackage {
    private SwitchPackage() {}

    /** The game builds the engine hooks are made for (src/runtime/hooks.cpp kBuilds), oldest first. */
    static final String[] SUPPORTED_BUILD_IDS = {
            "5a797a3a5da6c4f3468cb7c84e4dde63",  // 1.0.0
            "6569410c91c398e78ec76b418ee4cca8",  // 1.0.3
    };
    static final String[] SUPPORTED_VERSIONS = {"1.0.0", "1.0.3"};

    static boolean supported(String buildId) {
        return version(buildId) != null;
    }

    /** The game version of a supported build ("1.0.3"), or null. */
    static String version(String buildId) {
        for (int i = 0; i < SUPPORTED_BUILD_IDS.length; i++)
            if (buildId != null && buildId.startsWith(SUPPORTED_BUILD_IDS[i])) return SUPPORTED_VERSIONS[i];
        return null;
    }

    /** Bytes that cannot be read: the base game's own RomFS tables when it comes from an extracted folder. */
    static final class UnknownBytesException extends IOException {
        UnknownBytesException() {
            super("not available from the installed game");
        }
    }

    /** A file the game cannot start without: a RomFS that holds it is the game's. */
    private static final String GAME_MARKER = "dvdbnd0.bhd5";

    static final class InstallException extends Exception {
        final String title;
        InstallException(String title, String message) {
            super(message);
            this.title = title;
        }
    }

    interface Progress {
        void update(long done, long total, String file);
        boolean cancelled();
    }

    // ---- sources --------------------------------------------------------------------------------------------

    interface Source {
        long size() throws IOException;
        /** Reads exactly len bytes at pos. */
        void read(long pos, byte[] b, int off, int len) throws IOException;
    }

    static final class FileSource implements Source, Closeable {
        final String name;
        private final FileChannel mCh;
        private final Closeable mOwner;
        private final long mSize;

        FileSource(String name, FileChannel ch, Closeable owner) throws IOException {
            this.name = name;
            mCh = ch;
            mOwner = owner;
            mSize = ch.size();
        }

        @Override
        public long size() {
            return mSize;
        }

        @Override
        public void read(long pos, byte[] b, int off, int len) throws IOException {
            if (pos < 0 || pos + len > mSize) throw new IOException(name + ": read past the end (truncated file?)");
            ByteBuffer bb = ByteBuffer.wrap(b, off, len);
            while (bb.hasRemaining()) {
                int n = mCh.read(bb, pos + (bb.position() - off));
                if (n < 0) throw new IOException(name + ": unexpected end of file");
            }
        }

        @Override
        public void close() throws IOException {
            mCh.close();
            if (mOwner != null) mOwner.close();
        }
    }

    static final class Slice implements Source {
        final Source src;
        final long off, len;

        Slice(Source src, long off, long len) {
            this.src = src;
            this.off = off;
            this.len = len;
        }

        @Override
        public long size() {
            return len;
        }

        @Override
        public void read(long pos, byte[] b, int o, int n) throws IOException {
            if (pos < 0 || pos + n > len) throw new IOException("read outside a package entry");
            src.read(off + pos, b, o, n);
        }
    }

    private static byte[] read(Source s, long pos, int len) throws IOException {
        byte[] b = new byte[len];
        s.read(pos, b, 0, len);
        return b;
    }

    static long u64(byte[] b, int o) {
        long v = 0;
        for (int i = 7; i >= 0; i--) v = (v << 8) | (b[o + i] & 0xFF);
        return v;
    }

    static int u32(byte[] b, int o) {
        return (b[o] & 0xFF) | (b[o + 1] & 0xFF) << 8 | (b[o + 2] & 0xFF) << 16 | (b[o + 3] & 0xFF) << 24;
    }

    private static boolean magic(byte[] b, int o, String m) {
        for (int i = 0; i < m.length(); i++)
            if (b[o + i] != (byte) m.charAt(i)) return false;
        return true;
    }

    static String hex(byte[] b, int o, int n) {
        StringBuilder s = new StringBuilder();
        for (int i = 0; i < n; i++) s.append(String.format(Locale.ROOT, "%02x", b[o + i] & 0xFF));
        return s.toString();
    }

    private static boolean allZero(byte[] b, int o, int n) {
        for (int i = 0; i < n; i++)
            if (b[o + i] != 0) return false;
        return true;
    }

    // ---- keys -----------------------------------------------------------------------------------------------

    static final class Keys {
        final Map<String, byte[]> map = new HashMap<>();

        /** prod.keys: `name = hex` lines. */
        static Keys parse(Reader r) throws IOException {
            Keys k = new Keys();
            BufferedReader br = new BufferedReader(r);
            String line;
            while ((line = br.readLine()) != null) {
                int eq = line.indexOf('=');
                if (eq <= 0) continue;
                String name = line.substring(0, eq).trim().toLowerCase(Locale.ROOT);
                String v = line.substring(eq + 1).trim();
                if (v.length() % 2 != 0 || v.isEmpty()) continue;
                byte[] b = new byte[v.length() / 2];
                boolean ok = true;
                for (int i = 0; i < b.length && ok; i++) {
                    int hi = Character.digit(v.charAt(2 * i), 16), lo = Character.digit(v.charAt(2 * i + 1), 16);
                    ok = hi >= 0 && lo >= 0;
                    b[i] = (byte) (hi << 4 | lo);
                }
                if (ok) k.map.put(name, b);
            }
            return k;
        }

        byte[] need(String name, int len) throws InstallException {
            byte[] b = map.get(name);
            if (b == null || b.length != len)
                throw new InstallException("Key missing", "prod.keys has no " + name + ". Dump the keys again "
                        + "from your Switch (with its latest firmware) and select the new prod.keys.");
            return b;
        }
    }

    // ---- crypto ---------------------------------------------------------------------------------------------

    private static byte[] ecbDecrypt(byte[] key, byte[] data) throws GeneralSecurityException {
        Cipher c = Cipher.getInstance("AES/ECB/NoPadding");
        c.init(Cipher.DECRYPT_MODE, new SecretKeySpec(key, "AES"));
        return c.doFinal(data);
    }

    /** AES-XTS with Nintendo's tweak (the sector number big-endian), 0x200-byte sectors from sector 0. */
    static byte[] xtsDecrypt(byte[] key32, byte[] data, int sectorSize) throws GeneralSecurityException {
        Cipher k1 = Cipher.getInstance("AES/ECB/NoPadding"), k2 = Cipher.getInstance("AES/ECB/NoPadding");
        k1.init(Cipher.DECRYPT_MODE, new SecretKeySpec(Arrays.copyOfRange(key32, 0, 16), "AES"));
        k2.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(Arrays.copyOfRange(key32, 16, 32), "AES"));
        byte[] out = new byte[data.length];
        byte[] t = new byte[16], x = new byte[16];
        for (int s = 0; s * sectorSize < data.length; s++) {
            byte[] tw = new byte[16];
            long n = s;
            for (int i = 15; i >= 8; i--, n >>>= 8) tw[i] = (byte) n;
            k2.doFinal(tw, 0, 16, t, 0);
            for (int o = s * sectorSize; o < (s + 1) * sectorSize && o < data.length; o += 16) {
                for (int i = 0; i < 16; i++) x[i] = (byte) (data[o + i] ^ t[i]);
                k1.doFinal(x, 0, 16, x, 0);
                for (int i = 0; i < 16; i++) out[o + i] = (byte) (x[i] ^ t[i]);
                int carry = 0;  // t *= alpha in GF(2^128), little-endian
                for (int i = 0; i < 16; i++) {
                    int v = t[i] & 0xFF;
                    t[i] = (byte) ((v << 1) | carry);
                    carry = v >>> 7;
                }
                if (carry != 0) t[0] ^= (byte) 0x87;
            }
        }
        return out;
    }

    /** AES-CTR: counter = upper (64 bits) : absolute offset / 16. */
    static final class Ctr {
        private final Cipher mC;
        private final SecretKeySpec mKey;
        private final byte[] mIv = new byte[16];

        Ctr(byte[] key) throws GeneralSecurityException {
            mC = Cipher.getInstance("AES/CTR/NoPadding");
            mKey = new SecretKeySpec(key, "AES");
        }

        /** Decrypts b[off, off+len) in place; abs is 16-aligned (the data's offset in the NCA). */
        void apply(long upper, long abs, byte[] b, int off, int len) throws IOException {
            long blk = abs >>> 4;
            for (int i = 7; i >= 0; i--, upper >>>= 8) mIv[i] = (byte) upper;
            for (int i = 15; i >= 8; i--, blk >>>= 8) mIv[i] = (byte) blk;
            try {
                mC.init(Cipher.DECRYPT_MODE, mKey, new IvParameterSpec(mIv));
                int done = 0;
                while (done < len) {  // in steps, so no large output buffer is made
                    int n = Math.min(len - done, 1 << 20);
                    int w = mC.update(b, off + done, n, b, off + done);
                    if (w != n) throw new IOException("AES-CTR: short output");
                    done += n;
                }
            } catch (GeneralSecurityException e) {
                throw new IOException("AES-CTR: " + e.getMessage(), e);
            }
        }
    }

    // ---- containers -----------------------------------------------------------------------------------------

    static final class Entry {
        final String name;
        final long offset, size;

        Entry(String name, long offset, long size) {
            this.name = name;
            this.offset = offset;
            this.size = size;
        }
    }

    /** PFS0 (NSP, exefs) or HFS0 (XCI) at pos: the entries, offsets relative to the source. */
    static List<Entry> readPartition(Source s, long pos) throws IOException {
        byte[] h = read(s, pos, 16);
        boolean hfs = magic(h, 0, "HFS0");
        if (!hfs && !magic(h, 0, "PFS0")) throw new IOException("not a PFS0 / HFS0 partition");
        int n = u32(h, 4), strSize = u32(h, 8), es = hfs ? 0x40 : 0x18;
        if (n < 0 || n > 100000 || strSize < 0 || strSize > (16 << 20)) throw new IOException("bad partition header");
        byte[] t = read(s, pos + 16, n * es + strSize);
        long data = pos + 16 + (long) n * es + strSize;
        List<Entry> out = new ArrayList<>();
        for (int i = 0; i < n; i++) {
            int e = i * es;
            int no = u32(t, e + 16), end = n * es + no;
            int z = end;
            while (z < t.length && t[z] != 0) z++;
            out.add(new Entry(new String(t, end, z - end, StandardCharsets.UTF_8), data + u64(t, e), u64(t, e + 8)));
        }
        return out;
    }

    // ---- NCA ------------------------------------------------------------------------------------------------

    static final class Section {
        int index, fsType, hashType, encType;
        long start, end;  // absolute in the NCA
        byte[] fsh;  // the 0x200-byte FsHeader
        long ctrUpper;
    }

    static final class Nca {
        final String name;
        final Source src;
        byte[] hdr;
        int contentType, keyGen;
        long programId;
        byte[] rightsId;
        byte[] key;  // section key (AES-128-CTR)
        final List<Section> sections = new ArrayList<>();
        Ctr ctr;

        Nca(String name, Source src) {
            this.name = name;
            this.src = src;
        }

        boolean hasPatch() {
            for (Section s : sections)
                if (s.encType == 4) return true;
            return false;
        }
    }

    static Nca openNca(String name, Source src, Keys keys) throws IOException, InstallException {
        Nca n = new Nca(name, src);
        try {
            n.hdr = xtsDecrypt(keys.need("header_key", 32), read(src, 0, 0xC00), 0x200);
        } catch (GeneralSecurityException e) {
            throw new IOException(e);
        }
        if (!magic(n.hdr, 0x200, "NCA3"))
            throw new InstallException("Wrong keys", name + " could not be read with the header_key in prod.keys "
                    + "(or it is not a Switch content file). Check that prod.keys is the one dumped from your Switch.");
        n.contentType = n.hdr[0x205] & 0xFF;
        int g = Math.max(n.hdr[0x206] & 0xFF, n.hdr[0x220] & 0xFF);
        n.keyGen = g > 0 ? g - 1 : 0;
        n.programId = u64(n.hdr, 0x210);
        n.rightsId = Arrays.copyOfRange(n.hdr, 0x230, 0x240);
        for (int i = 0; i < 4; i++) {
            long st = (u32(n.hdr, 0x240 + i * 16) & 0xFFFFFFFFL) * 0x200;
            long en = (u32(n.hdr, 0x244 + i * 16) & 0xFFFFFFFFL) * 0x200;
            if (en <= st) continue;
            Section s = new Section();
            s.index = i;
            s.start = st;
            s.end = en;
            s.fsh = Arrays.copyOfRange(n.hdr, 0x400 + i * 0x200, 0x600 + i * 0x200);
            s.fsType = s.fsh[2] & 0xFF;
            s.hashType = s.fsh[3] & 0xFF;
            s.encType = s.fsh[4] & 0xFF;
            long gen = u32(s.fsh, 0x140) & 0xFFFFFFFFL, secure = u32(s.fsh, 0x144) & 0xFFFFFFFFL;
            s.ctrUpper = secure << 32 | gen;
            n.sections.add(s);
        }
        return n;
    }

    /** The section key: from the ticket's title key (rights ID) or the NCA's key area. */
    static void unlockNca(Nca n, Keys keys, Map<String, byte[]> titleKeys) throws IOException, InstallException {
        String gen = String.format(Locale.ROOT, "%02x", n.keyGen);
        try {
            if (!allZero(n.rightsId, 0, 16)) {
                String rid = hex(n.rightsId, 0, 16);
                byte[] tk = titleKeys.get(rid);
                if (tk == null)
                    throw new InstallException("Ticket missing", n.name + " needs the title key of rights ID " + rid
                            + ", but the package has no ticket for it.");
                n.key = ecbDecrypt(keys.need("titlekek_" + gen, 16), tk);
            } else {
                int kaek = n.hdr[0x207] & 0xFF;
                String kind = kaek == 1 ? "ocean" : kaek == 2 ? "system" : "application";
                byte[] area = ecbDecrypt(keys.need("key_area_key_" + kind + "_" + gen, 16),
                        Arrays.copyOfRange(n.hdr, 0x300, 0x340));
                n.key = Arrays.copyOfRange(area, 0x20, 0x30);
            }
            n.ctr = new Ctr(n.key);
        } catch (GeneralSecurityException e) {
            throw new IOException(e);
        }
    }

    /** A section's data, decrypted with its own counter (types 1 none, 3 CTR; 4 for the patch's meta tables). */
    static final class SectionSource implements Source {
        final Nca nca;
        final Section sec;
        private byte[] mTmp = new byte[0];

        SectionSource(Nca nca, Section sec) {
            this.nca = nca;
            this.sec = sec;
        }

        @Override
        public long size() {
            return sec.end - sec.start;
        }

        @Override
        public void read(long pos, byte[] b, int off, int len) throws IOException {
            if (pos < 0 || pos + len > size()) throw new IOException(nca.name + ": read outside section");
            if (sec.encType == 1) {
                nca.src.read(sec.start + pos, b, off, len);
                return;
            }
            if (sec.encType != 3 && sec.encType != 4)
                throw new IOException(nca.name + ": section encryption type " + sec.encType + " is not supported");
            decryptCtr(nca, sec.start + pos, sec.ctrUpper, b, off, len);
        }

        private void decryptCtr(Nca n, long abs, long upper, byte[] b, int off, int len) throws IOException {
            long a = abs & ~15L;
            int skip = (int) (abs - a), total = (skip + len + 15) & ~15;
            if (skip == 0 && total == len) {
                n.src.read(a, b, off, len);
                n.ctr.apply(upper, a, b, off, len);
                return;
            }
            if (mTmp.length < total) mTmp = new byte[total];
            long avail = n.src.size() - a;
            int rd = (int) Math.min(total, avail);
            n.src.read(a, mTmp, 0, rd);
            n.ctr.apply(upper, a, mTmp, 0, total);
            System.arraycopy(mTmp, skip, b, off, len);
        }
    }

    /** One BucketTree (BKTR) table: its entries (entrySize bytes each) in order, and the end offset. */
    static final class Bucket {
        final byte[] data;  // entries, back to back
        final int count, entrySize;
        final long end;

        Bucket(byte[] data, int count, int entrySize, long end) {
            this.data = data;
            this.count = count;
            this.entrySize = entrySize;
            this.end = end;
        }

        long key(int i) {
            return u64(data, i * entrySize);
        }

        /** The last entry whose offset is <= pos (entries are sorted). */
        int find(long pos) {
            int lo = 0, hi = count - 1, r = -1;
            while (lo <= hi) {
                int m = (lo + hi) >>> 1;
                if (key(m) <= pos) {
                    r = m;
                    lo = m + 1;
                } else {
                    hi = m - 1;
                }
            }
            return r;
        }
    }

    static Bucket readBucketTree(Source s, long off, long size, byte[] header, int entrySize, String what)
            throws IOException, InstallException {
        if (!magic(header, 0, "BKTR"))
            throw new InstallException("Update not supported", "The update's " + what + " table is not in a known format.");
        int total = u32(header, 8);
        if (total <= 0) return new Bucket(new byte[0], 0, entrySize, 0);
        if (size < 0x4000 || size > (512L << 20)) throw new IOException("bad " + what + " table size");
        byte[] t = read(s, off, (int) size);
        int sets = u32(t, 4);
        long end = u64(t, 8);
        int per = (0x4000 - 0x10) / entrySize;
        if (sets <= 0 || (long) sets * 0x4000 + 0x4000 > size || sets > (0x4000 - 0x10) / 8)
            throw new InstallException("Update not supported", "The update's " + what + " table is too large for this "
                    + "installer (" + total + " entries).");
        byte[] out = new byte[total * entrySize];
        int n = 0;
        for (int i = 0; i < sets; i++) {
            int base = 0x4000 + i * 0x4000;
            int c = u32(t, base + 4);
            if (c < 0 || c > per || n + c > total) throw new IOException("bad " + what + " table");
            System.arraycopy(t, base + 0x10, out, n * entrySize, c * entrySize);
            n += c;
        }
        return new Bucket(out, n, entrySize, end);
    }

    /** The RomFS of a game with its update: IndirectStorage over the base RomFS and the update's data. */
    static final class PatchedSource implements Source {
        private final Source mBase;
        private final Nca mPatch;
        private final Section mSec;
        final Bucket mInd;
        private final Bucket mCtrEx;
        private final long mSecure;
        private final SectionSource mPatchRaw;
        private byte[] mTmp = new byte[0];

        PatchedSource(Source base, Nca patch, Section sec) throws IOException, InstallException {
            mBase = base;
            mPatch = patch;
            mSec = sec;
            mPatchRaw = new SectionSource(patch, sec);  // the tables: normal section counter
            byte[] f = sec.fsh;
            mInd = readBucketTree(mPatchRaw, u64(f, 0x100), u64(f, 0x108), Arrays.copyOfRange(f, 0x110, 0x120),
                    0x14, "relocation");
            mCtrEx = readBucketTree(mPatchRaw, u64(f, 0x120), u64(f, 0x128), Arrays.copyOfRange(f, 0x130, 0x140),
                    0x10, "subsection");
            mSecure = u32(f, 0x144) & 0xFFFFFFFFL;
            if (mInd.count == 0) throw new InstallException("Update not supported", "The update has no RomFS patch.");
        }

        @Override
        public long size() {
            return mInd.end;
        }

        @Override
        public void read(long pos, byte[] b, int off, int len) throws IOException {
            while (len > 0) {
                int i = mInd.find(pos);
                if (i < 0 || pos >= mInd.end) throw new IOException("update RomFS: read outside the patch");
                long v = mInd.key(i), next = i + 1 < mInd.count ? mInd.key(i + 1) : mInd.end;
                long phys = u64(mInd.data, i * 0x14 + 8) + (pos - v);
                int idx = u32(mInd.data, i * 0x14 + 16);
                int n = (int) Math.min(len, next - pos);
                if (idx == 0)
                    mBase.read(phys, b, off, n);
                else
                    readPatch(phys, b, off, n);
                pos += n;
                off += n;
                len -= n;
            }
        }

        /** The update's own data: AES-CTR with the subsection's generation in the counter. */
        private void readPatch(long pos, byte[] b, int off, int len) throws IOException {
            while (len > 0) {
                int i = mCtrEx.find(pos);
                if (i < 0) throw new IOException("update RomFS: no subsection for offset " + pos);
                long next = i + 1 < mCtrEx.count ? mCtrEx.key(i + 1) : mCtrEx.end;
                int n = (int) Math.min(len, next - pos);
                int e = i * 0x10;
                boolean plain = mCtrEx.data[e + 8] == 1;  // encryption value 1: stored unencrypted
                long gen = u32(mCtrEx.data, e + 12) & 0xFFFFFFFFL;
                long abs = mSec.start + pos;
                if (plain) {
                    mPatch.src.read(abs, b, off, n);
                } else {
                    long a = abs & ~15L;
                    int skip = (int) (abs - a), total = (skip + n + 15) & ~15;
                    if (mTmp.length < total) mTmp = new byte[total];
                    int rd = (int) Math.min(total, mPatch.src.size() - a);
                    mPatch.src.read(a, mTmp, 0, rd);
                    mPatch.ctr.apply(mSecure << 32 | gen, a, mTmp, 0, total);
                    System.arraycopy(mTmp, skip, b, off, n);
                }
                pos += n;
                off += n;
                len -= n;
            }
        }
    }

    // ---- exefs / RomFS --------------------------------------------------------------------------------------

    private static MessageDigest sha256() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (GeneralSecurityException e) {
            throw new IllegalStateException(e);
        }
    }

    /** The exefs of a program NCA (the PFS0 section with `main`), checked against its master hash. */
    static Source exefs(Nca n) throws IOException, InstallException {
        for (Section s : n.sections) {
            if (s.fsType != 1 || s.hashType != 2) continue;
            SectionSource ss = new SectionSource(n, s);
            long htOff = u64(s.fsh, 0x30), htSize = u64(s.fsh, 0x38), pOff = u64(s.fsh, 0x40), pSize = u64(s.fsh, 0x48);
            if (htSize <= 0 || htSize > (16 << 20) || pOff + pSize > ss.size()) continue;
            byte[] ht = read(ss, htOff, (int) htSize);
            if (!Arrays.equals(sha256().digest(ht), Arrays.copyOfRange(s.fsh, 0x8, 0x28)))
                throw new InstallException("Wrong keys", n.name + " could not be decrypted: the keys in prod.keys do "
                        + "not match this game (key generation " + n.keyGen + "). Dump the keys again from your Switch.");
            Slice p = new Slice(ss, pOff, pSize);
            for (Entry e : readPartition(p, 0))
                if (e.name.equals("main")) return p;
        }
        throw new InstallException("Not a game", n.name + " has no program (exefs).");
    }

    /** The build ID of exefs/main (hex, the first 16 bytes). */
    static String buildId(Source exefs) throws IOException {
        for (Entry e : readPartition(exefs, 0))
            if (e.name.equals("main")) {
                byte[] h = read(exefs, e.offset, 0x60);
                if (!magic(h, 0, "NSO0")) throw new IOException("exefs/main is not an NSO");
                return hex(h, 0x40, 16);
            }
        throw new IOException("no exefs/main");
    }

    static final class RomFile {
        final String path;
        final long offset, size;  // in the RomFS source (level 6 data + table offsets applied)

        RomFile(String path, long offset, long size) {
            this.path = path;
            this.offset = offset;
            this.size = size;
        }
    }

    /** The RomFS section of a program NCA (base, or base + update), as a section-relative source. */
    static Source romfsSection(Nca base, Nca patch) throws IOException, InstallException {
        Section bs = null;
        for (Section s : base.sections)
            if (s.fsType == 0 && s.hashType == 3) bs = s;
        if (bs == null) throw new InstallException("Not a game", base.name + " has no RomFS.");
        if (!allZero(bs.fsh, 0x148, 0x20))
            throw new InstallException("Not supported", base.name + " uses sparse storage, which this installer "
                    + "cannot read.");
        Source src = new SectionSource(base, bs);
        if (patch == null) return src;
        Section ps = null;
        for (Section s : patch.sections)
            if (s.fsType == 0 && s.encType == 4) ps = s;
        if (ps == null) throw new InstallException("Update not supported", patch.name + " has no RomFS patch.");
        return new PatchedSource(src, patch, ps);
    }

    /** The FsHeader describing the RomFS image (the update's when patched: its IVFC covers the patched image). */
    static byte[] romfsHeader(Nca base, Nca patch) {
        Nca n = patch != null ? patch : base;
        for (Section s : n.sections)
            if (s.fsType == 0 && (patch == null ? s.hashType == 3 : s.encType == 4)) return s.fsh;
        return null;
    }

    /** The files of a RomFS image: level 6 of the IVFC tree, checked against the first hash of level 5. */
    static List<RomFile> romfsFiles(Source sec, byte[] fsh, String name) throws IOException, InstallException {
        if (fsh == null || !magic(fsh, 0x8, "IVFC")) throw new InstallException("Not a game", name + ": no RomFS.");
        int levels = u32(fsh, 0x14);  // including the master hash
        if (levels < 2 || levels > 7) throw new IOException("bad IVFC level count " + levels);
        int dl = levels - 2;
        long dataOff = u64(fsh, 0x18 + dl * 0x18), dataSize = u64(fsh, 0x20 + dl * 0x18);
        int blockLog = u32(fsh, 0x28 + dl * 0x18);
        if (dl > 0 && blockLog >= 9 && blockLog <= 24 && dataSize >= (1L << blockLog)) {
            long hOff = u64(fsh, 0x18 + (dl - 1) * 0x18);
            byte[] want, got;
            try {
                want = read(sec, hOff, 32);
                got = sha256().digest(read(sec, dataOff, 1 << blockLog));
            } catch (UnknownBytesException e) {  // checked block by block while unpacking
                want = got = null;
            }
            if (!Arrays.equals(want, got))
                throw new InstallException("Wrong keys", name + " could not be decrypted: the RomFS does not match its "
                        + "hashes. The keys in prod.keys may be wrong, or the base game and update do not belong "
                        + "together.");
        }
        byte[] h = read(sec, dataOff, 0x50);
        long dirOff = u64(h, 0x18), dirSize = u64(h, 0x20), fileOff = u64(h, 0x38), fileSize = u64(h, 0x40),
             dataBase = u64(h, 0x48);
        if (u64(h, 0) != 0x50 || dirSize > (64 << 20) || fileSize > (64 << 20))
            throw new IOException(name + ": bad RomFS header");
        byte[] dirs = read(sec, dataOff + dirOff, (int) dirSize), files = read(sec, dataOff + fileOff, (int) fileSize);
        List<RomFile> out = new ArrayList<>();
        walk(dirs, files, 0, "", dataOff + dataBase, out, 0);
        return out;
    }

    private static final int NONE = 0xFFFFFFFF;

    private static void walk(byte[] dirs, byte[] files, int dir, String path, long data, List<RomFile> out, int depth)
            throws IOException {
        if (depth > 64) throw new IOException("RomFS: directories too deep");
        int f = u32(dirs, dir + 12);
        int guard = 0;
        while (f != NONE) {
            if (f < 0 || f + 0x20 > files.length || ++guard > 1000000) throw new IOException("RomFS: bad file table");
            int nl = u32(files, f + 0x1C);
            String name = new String(files, f + 0x20, nl, StandardCharsets.UTF_8);
            safeName(name);
            out.add(new RomFile(path + name, data + u64(files, f + 8), u64(files, f + 16)));
            f = u32(files, f + 4);
        }
        int d = u32(dirs, dir + 8);
        while (d != NONE) {
            if (d < 0 || d + 0x18 > dirs.length || ++guard > 1000000) throw new IOException("RomFS: bad directory table");
            int nl = u32(dirs, d + 0x14);
            String name = new String(dirs, d + 0x18, nl, StandardCharsets.UTF_8);
            safeName(name);
            walk(dirs, files, d, path + name + "/", data, out, depth + 1);
            d = u32(dirs, d + 4);
        }
    }

    private static void safeName(String n) throws IOException {
        if (n.isEmpty() || n.equals(".") || n.equals("..") || n.contains("/") || n.contains("\\") || n.indexOf(0) >= 0)
            throw new IOException("RomFS: bad file name");
    }

    // ---- install --------------------------------------------------------------------------------------------

    static final class Result {
        String buildId, version;
        boolean withUpdate, supportedBuild, updateIgnored, overInstalled;
        long bytes, blocksVerified, blocksUnverified;
    }

    /** Ticket: the common title key of a rights ID (personalized tickets cannot be used). */
    static void readTicket(Source t, Map<String, byte[]> titleKeys) throws IOException {
        if (t.size() < 0x2C0) return;
        byte[] b = read(t, 0, (int) Math.min(t.size(), 0x400));
        int type = u32(b, 0);
        int sig;
        switch (type) {
            case 0x10000: case 0x10003: sig = 0x200; break;
            case 0x10001: case 0x10004: sig = 0x100; break;
            case 0x10002: case 0x10005: sig = 0x3C; break;
            default: return;
        }
        int d = (4 + sig + 0x3F) & ~0x3F;
        if (d + 0x170 > b.length) return;
        if ((b[d + 0x141] & 0xFF) != 0) return;  // personalized
        titleKeys.put(hex(b, d + 0x160, 16), Arrays.copyOfRange(b, d + 0x40, d + 0x50));
    }

    /** Collects the NCAs (and tickets) of an NSP or XCI. */
    static void scan(FileSource f, Keys keys, List<Nca> ncas, Map<String, byte[]> titleKeys)
            throws IOException, InstallException {
        String lower = f.name.toLowerCase(Locale.ROOT);
        if (lower.endsWith(".nsz") || lower.endsWith(".xcz"))
            throw new InstallException("Compressed package", f.name + " is compressed (NSZ / XCZ). Decompress it to "
                    + ".nsp / .xci first.");
        byte[] head = read(f, 0, (int) Math.min(f.size(), 0x1200));
        List<Entry> entries;
        if (magic(head, 0, "PFS0")) {
            entries = readPartition(f, 0);
        } else {
            long base = head.length >= 0x104 && magic(head, 0x100, "HEAD") ? 0
                    : head.length >= 0x1104 && magic(head, 0x1100, "HEAD") ? 0x1000 : -1;
            if (base < 0)
                throw new InstallException("Not a game package", f.name + " is not an .nsp or .xci file.");
            long hfs = base + u64(head, (int) base + 0x130);
            entries = null;
            for (Entry e : readPartition(f, hfs))
                if (e.name.equals("secure")) entries = readPartition(f, e.offset);
            if (entries == null) throw new InstallException("Not a game package", f.name + ": no secure partition.");
        }
        for (Entry e : entries) {
            String n = e.name.toLowerCase(Locale.ROOT);
            if (n.endsWith(".tik")) readTicket(new Slice(f, e.offset, e.size), titleKeys);
        }
        for (Entry e : entries) {
            String n = e.name.toLowerCase(Locale.ROOT);
            if (n.endsWith(".ncz"))
                throw new InstallException("Compressed package", f.name + " holds compressed content (NCZ). "
                        + "Decompress it to .nsp / .xci first.");
            if (!n.endsWith(".nca") || n.endsWith(".cnmt.nca") || e.size < 0xC00) continue;
            Nca nca = openNca(e.name, new Slice(f, e.offset, e.size), keys);
            if (nca.contentType == 0) ncas.add(nca);  // programs only
        }
    }

    /** The files of a game folder (relative paths with '/'), without exefs and our own files. */
    private static void listGame(File dir, String rel, List<String> out, int depth) {
        File[] l = dir.listFiles();
        if (l == null || depth > 16) return;
        for (File f : l) {
            String r = rel + f.getName();
            if (f.isDirectory()) {
                if (!(rel.isEmpty() && f.getName().equals("exefs"))) listGame(f, r + "/", out, depth + 1);
            } else if (f.isFile() && !f.getName().endsWith(".part")) {
                out.add(r);
            }
        }
    }

    /**
     * The base game's RomFS, rebuilt from an extracted game folder: the files in the order of their paths, each at
     * a 16-byte boundary (as the game's RomFS stores them). Where it starts in the base section ("anchor") is found
     * from the update's relocation table: the parts of unchanged files it takes from the base game.
     */
    static final class FolderBase implements Source, Closeable {
        private String[] mPaths = new String[0];
        private long[] mStart = new long[0], mSize = new long[0];
        private FileChannel[] mCh = new FileChannel[0];
        private File mDir;
        private long mAnchor = Long.MIN_VALUE;

        @Override
        public long size() {
            return Long.MAX_VALUE;
        }

        void init(File dir, List<RomFile> romFiles, Bucket ind) throws IOException, InstallException {
            mDir = dir;
            Map<String, RomFile> wanted = new HashMap<>();
            for (RomFile r : romFiles) wanted.put(r.path, r);
            List<String> found = new ArrayList<>();
            listGame(dir, "", found, 0);
            List<String> paths = new ArrayList<>();
            for (String f : found)
                if (wanted.containsKey(f)) paths.add(f);
            java.util.Collections.sort(paths);
            mPaths = paths.toArray(new String[0]);
            mStart = new long[mPaths.length];
            mSize = new long[mPaths.length];
            mCh = new FileChannel[mPaths.length];
            Map<String, Long> start = new HashMap<>();
            long off = 0;
            for (int i = 0; i < mPaths.length; i++) {
                off = (off + 15) & ~15L;
                mStart[i] = off;
                mSize[i] = new File(dir, mPaths[i]).length();
                start.put(mPaths[i], off);
                off += mSize[i];
            }
            // Votes: for each part taken from the base game inside a file present in both, where the base RomFS
            // data would start. Unchanged files agree; parts of a changed file can agree among themselves on a
            // wrong place (data moved inside it), so the place most files agree on wins, then the most bytes.
            List<RomFile> sorted = new ArrayList<>(romFiles);
            sorted.sort((a, b) -> Long.compare(a.offset, b.offset));
            Map<Long, Long> votes = new HashMap<>();
            Map<Long, java.util.Set<String>> voters = new HashMap<>();
            for (int i = 0; i < ind.count; i++) {
                if (u32(ind.data, i * 0x14 + 16) != 0) continue;
                long v = ind.key(i), next = i + 1 < ind.count ? ind.key(i + 1) : ind.end;
                long phys = u64(ind.data, i * 0x14 + 8);
                RomFile f = containing(sorted, v);
                if (f == null || !start.containsKey(f.path)) continue;
                long within = v - f.offset;
                long cand = phys - (start.get(f.path) + within);
                votes.merge(cand, Math.min(next - v, f.size - within), Long::sum);
                voters.computeIfAbsent(cand, k -> new java.util.HashSet<>()).add(f.path);
            }
            long best = 0, anchor = 0;
            int bestFiles = 0;
            for (Map.Entry<Long, Long> e : votes.entrySet()) {
                int nf = voters.get(e.getKey()).size();
                if (nf > bestFiles || (nf == bestFiles && e.getValue() > best)) {
                    bestFiles = nf;
                    best = e.getValue();
                    anchor = e.getKey();
                }
            }
            if (best == 0)
                throw new InstallException("Game folder does not match", "The update could not be matched with the "
                        + "game in " + dir + ". Install the base game from its .nsp / .xci together with the update.");
            mAnchor = anchor;
        }

        private static RomFile containing(List<RomFile> sorted, long pos) {
            int lo = 0, hi = sorted.size() - 1, r = -1;
            while (lo <= hi) {
                int m = (lo + hi) >>> 1;
                if (sorted.get(m).offset <= pos) {
                    r = m;
                    lo = m + 1;
                } else {
                    hi = m - 1;
                }
            }
            if (r < 0) return null;
            RomFile f = sorted.get(r);
            return pos < f.offset + f.size ? f : null;
        }

        @Override
        public void read(long pos, byte[] b, int off, int len) throws IOException {
            if (mAnchor == Long.MIN_VALUE) throw new UnknownBytesException();
            long q = pos - mAnchor;
            while (len > 0) {
                int lo = 0, hi = mPaths.length - 1, i = -1;
                while (lo <= hi) {
                    int m = (lo + hi) >>> 1;
                    if (mStart[m] <= q) {
                        i = m;
                        lo = m + 1;
                    } else {
                        hi = m - 1;
                    }
                }
                if (i < 0 || q < 0) throw new UnknownBytesException();
                long end = mStart[i] + mSize[i];
                int n;
                if (q < end) {
                    n = (int) Math.min(len, end - q);
                    if (mCh[i] == null) mCh[i] = new java.io.RandomAccessFile(new File(mDir, mPaths[i]), "r").getChannel();
                    ByteBuffer bb = ByteBuffer.wrap(b, off, n);
                    while (bb.hasRemaining())
                        if (mCh[i].read(bb, q - mStart[i] + (bb.position() - off)) < 0)
                            throw new IOException(mPaths[i] + " changed while installing");
                } else {  // the padding up to the next file: zeros
                    long next = i + 1 < mPaths.length ? mStart[i + 1] : -1;
                    if (next < 0 || q >= next) throw new UnknownBytesException();
                    n = (int) Math.min(len, next - q);
                    Arrays.fill(b, off, off + n, (byte) 0);
                }
                q += n;
                off += n;
                len -= n;
            }
        }

        @Override
        public void close() {
            for (FileChannel c : mCh)
                try {
                    if (c != null) c.close();
                } catch (IOException ignored) {
                }
        }
    }

    /** A folder that holds a complete extracted game (exefs/main and the data archives). */
    static boolean isGameFolder(File dir) {
        if (dir == null) return false;
        for (String f : new String[] {"exefs/main", "dvdbnd0.bhd5", "dvdbnd0.bdt", "dvdbnd1.bhd5", "dvdbnd1.bdt"})
            if (!new File(dir, f).isFile()) return false;
        return true;
    }

    /** The build ID of an extracted game's exefs/main, or null. */
    static String folderBuildId(File dir) {
        try (java.io.RandomAccessFile f = new java.io.RandomAccessFile(new File(dir, "exefs/main"), "r")) {
            byte[] h = new byte[0x60];
            f.readFully(h);
            return magic(h, 0, "NSO0") ? hex(h, 0x40, 16) : null;
        } catch (IOException e) {
            return null;
        }
    }

    /**
     * Writes exefs/ and the RomFS files of the game in `files` to `dest`: a base game (with its update, if given),
     * or an update alone over the extracted base game in `installed` (may be `dest` itself). Everything is
     * written to .part files first and put in place at the end, exefs/main last, so an interrupted install never
     * looks complete (and an update can read the old files while it writes the new ones).
     */
    static Result install(List<FileSource> files, Keys keys, File dest, File installed, Progress progress,
            long freeBytes) throws IOException, InstallException {
        List<Nca> ncas = new ArrayList<>();
        Map<String, byte[]> titleKeys = new HashMap<>();
        for (FileSource f : files) scan(f, keys, ncas, titleKeys);
        // Base programs (no patch) and updates (with a RomFS patch).
        List<Nca> bases = new ArrayList<>(), updates = new ArrayList<>();
        for (Nca n : ncas) (n.hasPatch() ? updates : bases).add(n);
        if (bases.isEmpty() && updates.isEmpty())
            throw new InstallException("No game found", "The selected files hold no game program.");
        if (bases.isEmpty() && !isGameFolder(installed))
            throw new InstallException("Base game missing", "Only the update was selected. Select the base game "
                    + "(.nsp or .xci) together with the update, or install the base game first.");
        // The base game whose RomFS holds the game's data archives.
        Nca base = null;
        List<RomFile> baseFiles = null;
        InstallException firstError = null;
        for (Nca n : bases) {
            List<RomFile> rf;
            try {
                unlockNca(n, keys, titleKeys);
                rf = romfsFiles(romfsSection(n, null), romfsHeader(n, null), n.name);
            } catch (InstallException e) {  // another program in the package (or no keys for it): try the next
                if (firstError == null) firstError = e;
                continue;
            }
            for (RomFile r : rf)
                if (r.path.equalsIgnoreCase(GAME_MARKER)) {
                    base = n;
                    baseFiles = rf;
                }
            if (base != null) break;
        }
        if (!bases.isEmpty() && base == null) {
            if (firstError != null) throw firstError;
            throw new InstallException("Not Dark Souls Remastered", "The selected game is not Dark Souls Remastered "
                    + "(its data files, such as " + GAME_MARKER + ", are missing).");
        }
        // The update: one whose build the hooks support, else any (the newest is not known without its metadata).
        Nca patch = null;
        String patchId = null;
        for (Nca u : updates) {
            if (base != null && (u.programId & ~0xFFFL) != (base.programId & ~0xFFFL)) continue;
            unlockNca(u, keys, titleKeys);
            String id = buildId(exefs(u));
            if (patch == null || (supported(id) && !supported(patchId))) {
                patch = u;
                patchId = id;
            }
        }
        Result res = new Result();
        Source exefs;
        if (base != null) {
            exefs = exefs(base);
            String baseId = buildId(exefs);
            res.buildId = baseId;
            if (patch != null && (supported(patchId) || !supported(baseId))) {
                exefs = exefs(patch);
                res.buildId = patchId;
                res.withUpdate = true;
            } else if (patch != null) {
                res.updateIgnored = true;  // the base game is a supported build, the update is not
                patch = null;
            }
        } else {
            if (patch == null)
                throw new InstallException("Not Dark Souls Remastered", "The selected update is not for this game.");
            exefs = exefs(patch);
            res.buildId = patchId;
            res.withUpdate = true;
            res.overInstalled = true;
        }
        res.supportedBuild = supported(res.buildId);
        res.version = version(res.buildId);

        Source rom;
        byte[] fsh;
        List<RomFile> romFiles;
        FolderBase folder = null;
        if (patch == null) {
            rom = romfsSection(base, null);
            fsh = romfsHeader(base, null);
            romFiles = baseFiles;
        } else if (base != null) {
            rom = romfsSection(base, patch);
            fsh = romfsHeader(base, patch);
            romFiles = romfsFiles(rom, fsh, patch.name);
        } else {
            Section ps = null;
            for (Section s : patch.sections)
                if (s.fsType == 0 && s.encType == 4) ps = s;
            if (ps == null) throw new InstallException("Update not supported", patch.name + " has no RomFS patch.");
            folder = new FolderBase();
            PatchedSource p = new PatchedSource(folder, patch, ps);
            fsh = ps.fsh;
            romFiles = romfsFiles(p, fsh, patch.name);
            folder.init(installed, romFiles, p.mInd);
            rom = p;
        }
        boolean found = false;
        for (RomFile r : romFiles) found |= r.path.equalsIgnoreCase(GAME_MARKER);
        if (!found) throw new InstallException("Not Dark Souls Remastered", "The game has no " + GAME_MARKER + ".");

        // Space: all new files are written before the old ones go.
        List<Entry> exe = readPartition(exefs, 0);
        long total = 0;
        for (RomFile r : romFiles) total += r.size;
        for (Entry e : exe) total += e.size;
        if (freeBytes >= 0 && total + (64L << 20) > freeBytes)
            throw new InstallException("Not enough space", String.format(Locale.ROOT, "Installing needs %.1f GB of "
                    + "free space, but only %.1f GB is free. Free up some space and try again.",
                    (total + (64L << 20)) / 1e9, freeBytes / 1e9));
        try {
            extract(rom, fsh, romFiles, exefs, exe, dest, progress, total, res, folder != null);
        } finally {
            if (folder != null) folder.close();
        }
        return res;
    }

    private static File part(File f) {
        return new File(f.getPath() + ".part");
    }

    private static void extract(Source rom, byte[] fsh, List<RomFile> romFiles, Source exefs, List<Entry> exe,
            File dest, Progress progress, long total, Result res, boolean fromFolder)
            throws IOException, InstallException {
        int levels = u32(fsh, 0x14), dl = levels - 2;
        long dataOff = u64(fsh, 0x18 + dl * 0x18), dataSize = u64(fsh, 0x20 + dl * 0x18);
        int blockLog = u32(fsh, 0x28 + dl * 0x18);
        boolean hashed = dl > 0 && blockLog >= 9 && blockLog <= 22;
        int bs = hashed ? 1 << blockLog : 1 << 14;
        long hOff = hashed ? u64(fsh, 0x18 + (dl - 1) * 0x18) : 0;
        int chunk = Math.max(bs, (4 << 20) / bs * bs);
        byte[] buf = new byte[chunk], tmp = new byte[1 << 20];
        List<RomFile> sorted = new ArrayList<>(romFiles);
        sorted.sort((a, b) -> Long.compare(a.offset, b.offset));
        List<File> written = new ArrayList<>();
        FileOutputStream[] outs = new FileOutputStream[sorted.size()];
        MessageDigest md = sha256();
        long done = 0;
        boolean ok = false;
        try {
            for (RomFile r : sorted) safePath(r.path);
            int first = 0;  // files before this one are complete
            for (int i = 0; i < sorted.size(); i++)
                if (sorted.get(i).size == 0) open(dest, sorted.get(i), written).close();
            long end = dataOff + dataSize;
            for (long pos = dataOff; pos < end && first < sorted.size(); ) {
                while (first < sorted.size() && sorted.get(first).offset + sorted.get(first).size <= pos) first++;
                if (first >= sorted.size()) break;
                if (sorted.get(first).offset >= pos + chunk) {  // skip to the next file, block aligned
                    pos = dataOff + (sorted.get(first).offset - dataOff) / bs * bs;
                    continue;
                }
                if (progress != null && progress.cancelled())
                    throw new InstallException("Cancelled", "The installation was cancelled.");
                int n = (int) Math.min(chunk, end - pos);
                boolean have;
                try {
                    rom.read(pos, buf, 0, n);
                    have = true;
                } catch (UnknownBytesException e) {
                    have = false;
                }
                int blocks = (n + bs - 1) / bs;
                if (have && hashed) {
                    byte[] want = null;
                    try {
                        want = read(rom, hOff + (pos - dataOff) / bs * 32, blocks * 32);
                    } catch (UnknownBytesException e) {
                        res.blocksUnverified += blocks;
                    }
                    for (int k = 0; want != null && k < blocks; k++) {
                        int len = Math.min(bs, n - k * bs);
                        md.update(buf, k * bs, len);
                        if (len < bs) md.update(new byte[bs - len]);  // the last block: zero padded
                        byte[] got = md.digest();
                        boolean match = Arrays.equals(got, Arrays.copyOfRange(want, k * 32, k * 32 + 32));
                        if (!match && len < bs) {
                            md.update(buf, k * bs, len);
                            match = Arrays.equals(md.digest(), Arrays.copyOfRange(want, k * 32, k * 32 + 32));
                        }
                        if (!match)
                            throw new InstallException(fromFolder ? "Game folder does not match" : "Damaged game file",
                                    fromFolder ? "The game files in " + dest + " are not the base game this update "
                                            + "belongs to (or are damaged). Install the base game from its .nsp / .xci "
                                            + "together with the update."
                                            : "The game data does not match its hashes (at offset " + (pos + k * bs)
                                            + "): the .nsp / .xci file is damaged, or the base game and update do not "
                                            + "belong together.");
                    }
                    if (want != null) res.blocksVerified += blocks;
                } else {
                    res.blocksUnverified += blocks;
                }
                for (int i = first; i < sorted.size(); i++) {
                    RomFile f = sorted.get(i);
                    if (f.offset >= pos + n) break;
                    long a = Math.max(pos, f.offset), b = Math.min(pos + n, f.offset + f.size);
                    if (b <= a) continue;
                    if (outs[i] == null) outs[i] = open(dest, f, written);
                    if (have) {
                        outs[i].write(buf, (int) (a - pos), (int) (b - a));
                    } else {
                        for (long q = a; q < b; ) {
                            int m = (int) Math.min(tmp.length, b - q);
                            try {
                                rom.read(q, tmp, 0, m);
                            } catch (UnknownBytesException e) {
                                throw new InstallException("Game folder does not match", "Part of " + f.path
                                        + " could not be rebuilt from the installed game. Install the base game from "
                                        + "its .nsp / .xci together with the update.");
                            }
                            outs[i].write(tmp, 0, m);
                            q += m;
                        }
                    }
                    done += b - a;
                    if (b == f.offset + f.size) {
                        outs[i].getFD().sync();
                        outs[i].close();
                        outs[i] = null;
                    }
                }
                if (progress != null) progress.update(done, total, sorted.get(Math.min(first, sorted.size() - 1)).path);
                pos += n;
            }
            for (int i = 0; i < outs.length; i++)
                if (outs[i] != null) throw new IOException(sorted.get(i).path + " is incomplete");
            if (fromFolder && res.blocksVerified == 0)
                throw new InstallException("Game folder does not match", "The update could not be checked against "
                        + "the game in " + dest + ".");
            // exefs
            Entry main = null;
            for (Entry e : exe) {
                safeName(e.name);
                if (e.name.equals("main")) main = e;
                File out = new File(dest, "exefs/" + e.name);
                try (FileOutputStream o = open(out, written)) {
                    for (long q = 0; q < e.size; ) {
                        int m = (int) Math.min(tmp.length, e.size - q);
                        exefs.read(e.offset + q, tmp, 0, m);
                        o.write(tmp, 0, m);
                        q += m;
                    }
                    o.getFD().sync();
                }
                done += e.size;
            }
            if (main == null) throw new InstallException("Not a game", "The game has no exefs/main.");
            // Into place: old exefs files of another version go, exefs/main last.
            new File(dest, "exefs/main").delete();
            File[] old = new File(dest, "exefs").listFiles();
            if (old != null)
                for (File o : old) {
                    boolean keep = o.getName().endsWith(".part");
                    for (Entry e : exe) keep |= e.name.equals(o.getName());
                    if (!keep && o.isFile()) o.delete();
                }
            File mainFile = new File(dest, "exefs/main");
            for (File p : written) {
                File f = new File(p.getPath().substring(0, p.getPath().length() - 5));
                if (!f.equals(mainFile)) commit(p, f);
            }
            commit(part(mainFile), mainFile);
            if (progress != null) progress.update(total, total, "exefs/main");
            res.bytes = done;
            ok = true;
        } finally {
            for (FileOutputStream o : outs)
                try {
                    if (o != null) o.close();
                } catch (IOException ignored) {
                }
            if (!ok)
                for (File p : written) p.delete();
        }
    }

    private static void safePath(String p) throws IOException {
        for (String s : p.split("/", -1)) safeName(s);
    }

    private static FileOutputStream open(File dest, RomFile f, List<File> written) throws IOException {
        return open(new File(dest, f.path), written);
    }

    private static FileOutputStream open(File out, List<File> written) throws IOException {
        File dir = out.getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot create " + dir);
        File p = part(out);
        written.add(p);
        return new FileOutputStream(p);
    }

    private static void commit(File part, File out) throws IOException {
        if (out.exists() && !out.delete()) throw new IOException("cannot replace " + out);
        if (!part.renameTo(out)) throw new IOException("cannot write " + out);
    }
}
