package com.nfsu1.androidevolved;

import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.util.zip.CRC32;
import java.util.zip.DataFormatException;
import java.util.zip.Inflater;
import java.util.zip.ZipException;

/** The streams the archive readers share: exact-length members with their checksum checked, raw Deflate. */
final class ArchiveStreams {
    private ArchiveStreams() {}

    /**
     * Exactly {@code size} bytes of {@code in}, never more. The CRC-32 of what was read is compared with {@code crc}
     * (when it is known, >= 0) as the last byte is handed out, and a source that ends early is a damaged member:
     * either way the caller never sees a member that does not match its archive's description of it.
     */
    static final class Exact extends InputStream {
        private final InputStream in;
        private final long size, crc;
        private final String name;
        private final CRC32 checksum;
        private long remaining;
        private boolean checked;

        Exact(InputStream in, long size, long crc, String name) {
            this.in = in;
            this.size = size;
            this.crc = crc;
            this.name = name;
            this.remaining = size;
            this.checksum = crc >= 0 ? new CRC32() : null;
        }

        long remaining() { return remaining; }

        @Override public int read() throws IOException {
            byte[] one = new byte[1];
            return read(one, 0, 1) == 1 ? one[0] & 0xFF : -1;
        }

        @Override public int read(byte[] buffer, int offset, int length) throws IOException {
            if (length == 0) return 0;
            if (remaining == 0) { check(); return -1; }
            int n;
            try {
                n = in.read(buffer, offset, (int) Math.min(length, remaining));
            } catch (IOException error) {
                throw ArchiveException.whileReading(error, name);
            }
            if (n < 0) {
                ArchiveException early = new ArchiveException(ArchiveException.Kind.DAMAGED_FILE, name, "ends " + remaining + " of " + size + " bytes early");
                throw early;
            }
            if (checksum != null) checksum.update(buffer, offset, n);
            remaining -= n;
            if (remaining == 0) check();
            return n;
        }

        @Override public long skip(long count) throws IOException {
            byte[] scratch = new byte[(int) Math.min(65536, Math.max(1, count))];
            long done = 0;
            while (done < count) {
                int n = read(scratch, 0, (int) Math.min(scratch.length, count - done));
                if (n < 0) break;
                done += n;
            }
            return done;
        }

        /** Reads what the consumer left (a member must be read to its end to be checked, and to reach the next one). */
        void drain() throws IOException {
            byte[] scratch = new byte[65536];
            while (read(scratch, 0, scratch.length) >= 0) { }
        }

        private void check() throws ArchiveException {
            if (checked) return;
            checked = true;
            if (checksum != null && checksum.getValue() != crc)
                throw new ArchiveException(ArchiveException.Kind.DAMAGED_FILE, name,
                        String.format("CRC %08x, the archive says %08x", checksum.getValue(), crc));
        }

        @Override public int available() throws IOException { return (int) Math.min(remaining, in.available()); }

        @Override public void close() throws IOException { in.close(); }
    }

    /**
     * Raw Deflate (ZIP method 8, 7z's Deflate coder). Like java.util.zip's own ZIP reader it feeds the inflater one
     * zero byte after the input ends: raw streams need it to see their final block through.
     */
    static final class Inflating extends InputStream {
        private final InputStream in;
        private final Inflater inflater = new Inflater(true);
        private final byte[] input = new byte[1 << 16];
        private boolean padded, ended;

        Inflating(InputStream in) { this.in = in; }

        @Override public int read() throws IOException {
            byte[] one = new byte[1];
            return read(one, 0, 1) == 1 ? one[0] & 0xFF : -1;
        }

        @Override public int read(byte[] buffer, int offset, int length) throws IOException {
            if (length == 0) return 0;
            if (ended) throw new IOException("stream closed");
            try {
                for (;;) {
                    int n = inflater.inflate(buffer, offset, length);
                    if (n > 0) return n;
                    if (inflater.finished()) return -1;
                    if (inflater.needsDictionary()) throw new ZipException("a preset dictionary is needed");
                    if (inflater.needsInput()) {
                        int got = in.read(input, 0, input.length);
                        if (got < 0) {
                            if (padded) throw new EOFException("deflate data ends early");
                            input[0] = 0;
                            got = 1;
                            padded = true;
                        }
                        inflater.setInput(input, 0, got);
                    }
                }
            } catch (DataFormatException error) {
                ZipException wrapped = new ZipException("invalid deflate data: " + error.getMessage());
                wrapped.initCause(error);
                throw wrapped;
            }
        }

        @Override public void close() throws IOException {
            if (!ended) { ended = true; inflater.end(); }
            in.close();
        }
    }

    /** Counts what passes through, for progress. */
    static final class Counting extends InputStream {
        private final InputStream in;
        long count;

        Counting(InputStream in) { this.in = in; }

        @Override public int read() throws IOException { int b = in.read(); if (b >= 0) count++; return b; }

        @Override public int read(byte[] buffer, int offset, int length) throws IOException {
            int n = in.read(buffer, offset, length);
            if (n > 0) count += n;
            return n;
        }

        @Override public void close() throws IOException { in.close(); }
    }

    /** Reads and drops {@code count} bytes (decoders cannot seek). */
    static void discard(InputStream in, long count, byte[] scratch) throws IOException {
        while (count > 0) {
            int n = in.read(scratch, 0, (int) Math.min(scratch.length, count));
            if (n < 0) throw new EOFException(count + " bytes missing");
            count -= n;
        }
    }

    static void readFully(InputStream in, byte[] buffer, int offset, int length) throws IOException {
        while (length > 0) {
            int n = in.read(buffer, offset, length);
            if (n < 0) throw new EOFException(length + " bytes missing");
            offset += n;
            length -= n;
        }
    }
}
