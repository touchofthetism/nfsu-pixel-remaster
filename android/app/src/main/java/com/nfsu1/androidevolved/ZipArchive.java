package com.nfsu1.androidevolved;

import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.zip.CRC32;

/**
 * A ZIP read through its central directory, with random access, and forgiving where real downloads are not tidy.
 *
 * java.util.zip.ZipInputStream walks the local headers front to back and trusts each one's sizes. archive.org's
 * "download all" ZIPs break that: they are assembled on the fly from each file's recorded size and CRC, so a file that
 * changed after it was recorded (an item's reviews.xml when someone adds a review) is streamed at its new length under
 * the old header. Every later member then sits that many bytes past where the central directory puts it, and the
 * stale member fails its CRC. Desktop tools cope by measuring the shift; so does this reader:
 *
 * - The central directory is found from the end record and its real position compared with the stated one; the
 *   difference (bytes inserted before it: a prefix, a member that grew) is tried for every member.
 * - Each member's local header is looked for where the directory says, then shifted by the running difference, then by
 *   the total, then searched for nearby (bounded); its name must match. A member whose header cannot be found is only
 *   an error if the importer needs that member.
 * - Members are read by their central directory sizes and CRC-checked (ArchiveStreams.Exact), STORED or DEFLATED.
 *   A STORED member can be opened as a ByteSource of its own, which is how a 7z inside the ZIP is read in place.
 * - ZIP64 (sizes, offsets, end record), UTF-8 and Info-ZIP Unicode names; '\' separators become '/'.
 */
final class ZipArchive {
    static final int STORED = 0, DEFLATED = 8;
    private static final int LOCAL = 0x04034b50, CENTRAL = 0x02014b50, END = 0x06054b50, END64 = 0x06064b50, LOCATOR64 = 0x07064b50;
    /** How far from the expected place a local header is searched for. */
    private static final long SEARCH_SLACK = 64 * 1024, SEARCH_MAX = 16L * 1024 * 1024;
    private static final int MAX_CENTRAL_DIRECTORY = 64 * 1024 * 1024;
    private static final int BUFFER = 256 * 1024;

    static final class Entry {
        final String name;            // '/' separated, as stored
        final byte[] rawName;
        final int flags, method;
        final long crc, compressedSize, size;
        final long headerOffset;      // where the central directory puts the local header
        final int externalAttributes, madeBy;
        long dataOffset = -1;         // where the data really starts; -1 when its local header was not found

        Entry(String name, byte[] rawName, int flags, int method, long crc, long compressedSize, long size, long headerOffset, int externalAttributes, int madeBy) {
            this.name = name; this.rawName = rawName; this.flags = flags; this.method = method; this.crc = crc;
            this.compressedSize = compressedSize; this.size = size; this.headerOffset = headerOffset;
            this.externalAttributes = externalAttributes; this.madeBy = madeBy;
        }

        boolean isDirectory() {
            if (name.endsWith("/")) return true;
            if (size != 0) return false;
            // Entries without the slash: the MS-DOS directory attribute (FAT, NTFS, VFAT writers) or a Unix mode.
            int host = madeBy >>> 8;
            if (host == 0 || host == 10 || host == 14) return (externalAttributes & 0x10) != 0;
            if (host == 3 || host == 19) return ((externalAttributes >>> 16) & 0xF000) == 0x4000;
            return false;
        }

        boolean encrypted() { return (flags & 1) != 0 || method == 99; }

        @Override public String toString() { return name; }
    }

    final ByteSource source;
    final String archiveName;
    final List<Entry> entries;
    /** Real position of the central directory minus the stated one. */
    final long shift;
    /** Members whose local header sat somewhere else than the central directory says (for the log). */
    final int moved;

    private ZipArchive(ByteSource source, String archiveName, List<Entry> entries, long shift, int moved) {
        this.source = source; this.archiveName = archiveName; this.entries = entries; this.shift = shift; this.moved = moved;
    }

    /** The source as a ZIP, or null when it has no end of central directory record (not a ZIP). */
    static ZipArchive open(ByteSource source, String archiveName, int maxEntries) throws IOException {
        long size = source.size();
        if (size < 22) return null;
        int tailLength = (int) Math.min(size, 22 + 0xFFFF + 20);
        long tailStart = size - tailLength;
        byte[] tail = source.bytes(tailStart, tailLength);
        int end = -1;
        for (int i = tailLength - 22; i >= 0; i--) {
            if (u32(tail, i) == END && i + 22 + u16(tail, i + 20) <= tailLength) { end = i; break; }
        }
        if (end < 0) return null;
        long endPosition = tailStart + end;
        int disk = u16(tail, end + 4), directoryDisk = u16(tail, end + 6);
        long count = u16(tail, end + 10);
        long directorySize = u32(tail, end + 12) & 0xFFFFFFFFL;
        long directoryOffset = u32(tail, end + 16) & 0xFFFFFFFFL;
        long directoryEnd = endPosition;
        // ZIP64: a locator right before the end record points at the ZIP64 end record (itself possibly shifted).
        if (end >= 20 && u32(tail, end - 20) == LOCATOR64) {
            long stated = u64(tail, end - 20 + 8);
            long position = findEnd64(source, stated, endPosition - 20);
            if (position >= 0) {
                byte[] record = source.bytes(position, 56);
                disk = (int) u32(record, 16);
                directoryDisk = (int) u32(record, 20);
                count = u64(record, 32);
                directorySize = u64(record, 40);
                directoryOffset = u64(record, 48);
                directoryEnd = position;
            }
        }
        if (disk != 0 || directoryDisk != 0) throw new ArchiveException(ArchiveException.Kind.SPLIT, archiveName);
        if (count > maxEntries) throw new ArchiveException(ArchiveException.Kind.LIMIT, archiveName, count + " entries");
        if (directorySize > MAX_CENTRAL_DIRECTORY || directorySize > directoryEnd)
            throw ArchiveException.damagedIndex(archiveName, "central directory size " + directorySize);
        // Where the central directory really is: right before the end record(s). The difference to the stated offset
        // is the shift of everything that moved (a prefix, or members that grew while the ZIP was assembled).
        long realStart = directoryEnd - directorySize;
        long shift = realStart - directoryOffset;
        byte[] directory = source.bytes(realStart, (int) directorySize);
        if (count > 0 && (directory.length < 4 || u32(directory, 0) != CENTRAL)) {
            // Not where the end record implies: trust the stated offset if a directory starts there.
            if (directoryOffset + directorySize <= size && u32(source.bytes(directoryOffset, 4), 0) == CENTRAL) {
                directory = source.bytes(directoryOffset, (int) directorySize);
                shift = 0;
            } else {
                throw ArchiveException.damagedIndex(archiveName, "no central directory at " + realStart + " or " + directoryOffset);
            }
        }
        List<Entry> entries = new ArrayList<>();
        int p = 0;
        while (p + 46 <= directory.length && u32(directory, p) == CENTRAL) {
            if (entries.size() >= maxEntries) throw new ArchiveException(ArchiveException.Kind.LIMIT, archiveName, "more than " + maxEntries + " entries");
            int madeBy = u16(directory, p + 4);
            int flags = u16(directory, p + 8), method = u16(directory, p + 10);
            long crc = u32(directory, p + 16) & 0xFFFFFFFFL;
            long compressed = u32(directory, p + 20) & 0xFFFFFFFFL, uncompressed = u32(directory, p + 24) & 0xFFFFFFFFL;
            int nameLength = u16(directory, p + 28), extraLength = u16(directory, p + 30), commentLength = u16(directory, p + 32);
            int external = (int) u32(directory, p + 38);
            long offset = u32(directory, p + 42) & 0xFFFFFFFFL;
            int nameStart = p + 46, extraStart = nameStart + nameLength, next = extraStart + extraLength + commentLength;
            if (next > directory.length) throw ArchiveException.damagedIndex(archiveName, "central directory entry " + entries.size());
            byte[] rawName = Arrays.copyOfRange(directory, nameStart, extraStart);
            String unicodeName = null;
            for (int x = extraStart; x + 4 <= extraStart + extraLength; ) {
                int id = u16(directory, x), length = u16(directory, x + 2), body = x + 4;
                if (body + length > extraStart + extraLength) break;
                if (id == 0x0001) {
                    int v = body;
                    if (uncompressed == 0xFFFFFFFFL && v + 8 <= body + length) { uncompressed = u64(directory, v); v += 8; }
                    if (compressed == 0xFFFFFFFFL && v + 8 <= body + length) { compressed = u64(directory, v); v += 8; }
                    if (offset == 0xFFFFFFFFL && v + 8 <= body + length) { offset = u64(directory, v); }
                } else if (id == 0x7075 && length >= 5 && directory[body] == 1) {
                    CRC32 check = new CRC32();
                    check.update(rawName);
                    if ((u32(directory, body + 1) & 0xFFFFFFFFL) == check.getValue())
                        unicodeName = new String(directory, body + 5, length - 5, StandardCharsets.UTF_8);
                }
                x = body + length;
            }
            if (compressed < 0 || uncompressed < 0 || offset < 0) throw ArchiveException.damagedIndex(archiveName, "sizes of entry " + entries.size());
            String name = unicodeName != null ? unicodeName : decodeName(rawName, (flags & 0x800) != 0);
            entries.add(new Entry(name.replace('\\', '/'), rawName, flags, method, crc, compressed, uncompressed, offset, external, madeBy));
            p = next;
        }
        // Fewer entries than counted means a cut-off directory (outside ZIP64 the count is 16 bits: 0xFFFF or wrapped).
        if (entries.size() < count && count != 0xFFFF)
            throw ArchiveException.damagedIndex(archiveName, entries.size() + " of " + count + " entries readable");
        int moved = locateLocalHeaders(source, entries, shift);
        return new ZipArchive(source, archiveName, Collections.unmodifiableList(entries), shift, moved);
    }

    private static long findEnd64(ByteSource source, long stated, long locatorPosition) throws IOException {
        if (stated >= 0 && stated + 56 <= locatorPosition && u32(source.bytes(stated, 4), 0) == END64) return stated;
        long direct = locatorPosition - 56;
        if (direct >= 0 && u32(source.bytes(direct, 4), 0) == END64) return direct;
        long from = Math.max(0, locatorPosition - 65536);
        byte[] window = source.bytes(from, (int) (locatorPosition - from));
        for (int i = window.length - 56; i >= 0; i--) if (u32(window, i) == END64) return from + i;
        return -1;
    }

    /** Finds each member's local header (see the class comment); returns how many were not where stated. */
    private static int locateLocalHeaders(ByteSource source, List<Entry> entries, long totalShift) throws IOException {
        List<Entry> byOffset = new ArrayList<>(entries);
        byOffset.sort((a, b) -> Long.compare(a.headerOffset, b.headerOffset));
        long running = 0;
        int moved = 0;
        byte[] header = new byte[30];
        for (Entry entry : byOffset) {
            long found = -1;
            for (long guess : new long[] {entry.headerOffset + running, entry.headerOffset + totalShift, entry.headerOffset}) {
                if (localHeaderAt(source, guess, entry, header) >= 0) { found = guess; break; }
            }
            if (found < 0) {
                long low = entry.headerOffset + Math.min(0, Math.min(running, totalShift)) - SEARCH_SLACK;
                long high = entry.headerOffset + Math.max(0, Math.max(running, totalShift)) + SEARCH_SLACK;
                found = search(source, Math.max(0, low), Math.min(high, Math.max(0, low) + SEARCH_MAX), entry, header);
            }
            if (found < 0) continue;            // unreadable member; an error only if someone opens it
            if (found != entry.headerOffset) moved++;
            running = found - entry.headerOffset;
            entry.dataOffset = localHeaderAt(source, found, entry, header);
        }
        return moved;
    }

    /** The data offset when a local header for {@code entry} starts at {@code position}, else -1. */
    private static long localHeaderAt(ByteSource source, long position, Entry entry, byte[] header) throws IOException {
        if (position < 0 || position + 30 > source.size()) return -1;
        if (source.read(position, header, 0, 30) != 30 || u32(header, 0) != LOCAL) return -1;
        int nameLength = u16(header, 26), extraLength = u16(header, 28);
        if (nameLength != entry.rawName.length || position + 30 + nameLength > source.size()) return -1;
        if (!Arrays.equals(source.bytes(position + 30, nameLength), entry.rawName)) return -1;
        return position + 30 + nameLength + extraLength;
    }

    private static long search(ByteSource source, long from, long to, Entry entry, byte[] header) throws IOException {
        byte[] chunk = new byte[65536];
        for (long base = from; base < to; base += chunk.length - 3) {
            int n = source.read(base, chunk, 0, (int) Math.min(chunk.length, to - base + 3));
            if (n < 4) break;
            for (int i = 0; i + 4 <= n; i++) {
                if (chunk[i] == 'P' && chunk[i + 1] == 'K' && chunk[i + 2] == 3 && chunk[i + 3] == 4
                        && localHeaderAt(source, base + i, entry, header) >= 0) return base + i;
            }
        }
        return -1;
    }

    /** The member's data, exactly its size, CRC-checked as it is read. */
    InputStream open(Entry entry) throws IOException {
        if (entry.encrypted()) throw new ArchiveException(ArchiveException.Kind.ENCRYPTED, archiveName);
        if (entry.dataOffset < 0) throw new ArchiveException(ArchiveException.Kind.DAMAGED_FILE, entry.name, "no local header");
        if (entry.dataOffset + entry.compressedSize > source.size())
            throw new ArchiveException(ArchiveException.Kind.DAMAGED_FILE, entry.name, "the archive ends inside it");
        InputStream raw = source.stream(entry.dataOffset, entry.compressedSize, (int) Math.min(BUFFER, Math.max(4096, entry.compressedSize)));
        InputStream data;
        switch (entry.method) {
            case STORED:
                if (entry.compressedSize != entry.size) throw new ArchiveException(ArchiveException.Kind.DAMAGED_FILE, entry.name, "stored with two sizes");
                data = raw;
                break;
            case DEFLATED:
                data = new ArchiveStreams.Inflating(raw);
                break;
            default:
                throw new ArchiveException(ArchiveException.Kind.UNSUPPORTED, archiveName, methodName(entry.method));
        }
        return new ArchiveStreams.Exact(data, entry.size, entry.crc, entry.name);
    }

    /** A STORED member as a source of its own (an archive inside the archive, read in place), or null. */
    ByteSource stored(Entry entry) {
        if (entry.method != STORED || entry.encrypted() || entry.dataOffset < 0 || entry.compressedSize != entry.size
                || entry.dataOffset + entry.size > source.size()) return null;
        return source.slice(entry.dataOffset, entry.size);
    }

    static String methodName(int method) {
        switch (method) {
            case 1: return "Shrink";
            case 6: return "Implode";
            case 9: return "Deflate64";
            case 12: return "BZip2";
            case 14: return "LZMA";
            case 93: return "Zstandard";
            case 95: return "XZ";
            case 98: return "PPMd";
            default: return "method " + method;
        }
    }

    private static final String CP437_HIGH =
            "ÇüéâäàåçêëèïîìÄÅ"
            + "ÉæÆôöòûùÿÖÜ¢£¥₧ƒ"
            + "áíóúñÑªº¿⌐¬½¼¡«»"
            + "░▒▓│┤╡╢╖╕╣║╗╝╜╛┐"
            + "└┴┬├─┼╞╟╚╔╩╦╠═╬╧"
            + "╨╤╥╙╘╒╓╫╪┘┌█▄▌▐▀"
            + "αßΓπΣσµτΦΘΩδ∞φε∩"
            + "≡±≥≤⌠⌡÷≈°∙·√ⁿ²■ ";

    /** UTF-8 when flagged or when the bytes are valid UTF-8 (many writers do not flag it), else code page 437. */
    static String decodeName(byte[] raw, boolean utf8) {
        boolean ascii = true;
        for (byte b : raw) if (b < 0) { ascii = false; break; }
        if (ascii) return new String(raw, StandardCharsets.US_ASCII);
        if (utf8) return new String(raw, StandardCharsets.UTF_8);
        try {
            return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(raw)).toString();
        } catch (CharacterCodingException notUtf8) {
            StringBuilder name = new StringBuilder(raw.length);
            for (byte b : raw) name.append(b >= 0 ? (char) b : CP437_HIGH.charAt((b & 0xFF) - 0x80));
            return name.toString();
        }
    }

    static int u16(byte[] b, int i) { return (b[i] & 0xFF) | (b[i + 1] & 0xFF) << 8; }
    static long u32(byte[] b, int i) { return (b[i] & 0xFFL) | (b[i + 1] & 0xFFL) << 8 | (b[i + 2] & 0xFFL) << 16 | (b[i + 3] & 0xFFL) << 24; }
    static long u64(byte[] b, int i) { return u32(b, i) | u32(b, i + 4) << 32; }
}
