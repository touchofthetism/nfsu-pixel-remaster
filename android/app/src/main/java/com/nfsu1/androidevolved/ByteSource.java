package com.nfsu1.androidevolved;

import java.io.Closeable;
import java.io.EOFException;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;

/**
 * Random access to the bytes of an archive: a file, the descriptor a document provider handed out, or a window
 * into one of those (a 7z stored inside a ZIP is read in place, never copied out). Reads are positional, so several
 * streams over the same source (the four inputs of a BCJ2 coder) can be open at once.
 */
abstract class ByteSource implements Closeable {
    /** The source's length in bytes. */
    abstract long size();

    /** Up to {@code length} bytes at {@code position}; fewer only at the end of the source, -1 past it. */
    abstract int read(long position, byte[] buffer, int offset, int length) throws IOException;

    @Override public void close() throws IOException { }

    final void readFully(long position, byte[] buffer, int offset, int length) throws IOException {
        while (length > 0) {
            int n = read(position, buffer, offset, length);
            if (n <= 0) throw new EOFException("the archive ends at byte " + position);
            position += n;
            offset += n;
            length -= n;
        }
    }

    final byte[] bytes(long position, int length) throws IOException {
        byte[] data = new byte[length];
        readFully(position, data, 0, length);
        return data;
    }

    /** The bytes [offset, offset + length) as a source of their own (positions start at 0). */
    final ByteSource slice(long offset, long length) {
        if (offset < 0 || length < 0 || offset > size() || length > size() - offset)
            throw new IllegalArgumentException("slice " + offset + "+" + length + " of " + size());
        return new Slice(this, offset, length);
    }

    /** Sequential reads of [offset, offset + length) through a buffer of its own; reading past the window ends the stream. */
    final InputStream stream(long offset, long length, int bufferSize) {
        if (offset < 0 || length < 0) throw new IllegalArgumentException("stream " + offset + "+" + length);
        return new Window(this, offset, length, bufferSize);
    }

    static ByteSource of(File file) throws IOException {
        RandomAccessFile handle = new RandomAccessFile(file, "r");
        try {
            return of(handle.getChannel(), handle);
        } catch (IOException | RuntimeException error) {
            handle.close();
            throw error;
        }
    }

    /** A channel that supports positional reads (a regular file). {@code owner} is closed with the source. */
    static ByteSource of(FileChannel channel, Closeable owner) throws IOException {
        return new Channel(channel, channel.size(), owner);
    }

    private static final class Channel extends ByteSource {
        private final FileChannel channel;
        private final long size;
        private final Closeable owner;

        Channel(FileChannel channel, long size, Closeable owner) { this.channel = channel; this.size = size; this.owner = owner; }

        @Override long size() { return size; }

        @Override int read(long position, byte[] buffer, int offset, int length) throws IOException {
            if (position >= size) return -1;
            if (length == 0) return 0;
            length = (int) Math.min(length, size - position);
            ByteBuffer target = ByteBuffer.wrap(buffer, offset, length);
            int total = 0;
            while (target.hasRemaining()) {
                int n = channel.read(target, position + total);
                if (n < 0) break;
                total += n;
            }
            return total == 0 ? -1 : total;
        }

        @Override public void close() throws IOException {
            try { channel.close(); } finally { if (owner != null) owner.close(); }
        }
    }

    private static final class Slice extends ByteSource {
        private final ByteSource parent;
        private final long offset, size;

        Slice(ByteSource parent, long offset, long size) { this.parent = parent; this.offset = offset; this.size = size; }

        @Override long size() { return size; }

        @Override int read(long position, byte[] buffer, int off, int length) throws IOException {
            if (position < 0) throw new IOException("negative position");
            if (position >= size) return -1;
            return parent.read(offset + position, buffer, off, (int) Math.min(length, size - position));
        }
    }

    private static final class Window extends InputStream {
        private final ByteSource source;
        private final long end;
        private long position;        // of the next byte the buffer would hold
        private final byte[] buffer;
        private int next, filled;     // buffer[next, filled) is unread

        Window(ByteSource source, long offset, long length, int bufferSize) {
            this.source = source;
            this.position = offset;
            this.end = offset + length;
            this.buffer = new byte[Math.max(4096, bufferSize)];
        }

        private boolean fill() throws IOException {
            if (position >= end) return false;
            int n = source.read(position, buffer, 0, (int) Math.min(buffer.length, end - position));
            if (n <= 0) throw new EOFException("the archive ends at byte " + position + " (" + (end - position) + " more were expected)");
            position += n;
            next = 0;
            filled = n;
            return true;
        }

        @Override public int read() throws IOException {
            if (next == filled && !fill()) return -1;
            return buffer[next++] & 0xFF;
        }

        @Override public int read(byte[] target, int offset, int length) throws IOException {
            if (length == 0) return 0;
            if (next == filled) {
                // Large reads skip the buffer.
                if (length >= buffer.length && position < end) {
                    int n = source.read(position, target, offset, (int) Math.min(length, end - position));
                    if (n <= 0) throw new EOFException("the archive ends at byte " + position + " (" + (end - position) + " more were expected)");
                    position += n;
                    return n;
                }
                if (!fill()) return -1;
            }
            int n = Math.min(length, filled - next);
            System.arraycopy(buffer, next, target, offset, n);
            next += n;
            return n;
        }

        @Override public long skip(long count) throws IOException {
            if (count <= 0) return 0;
            long buffered = filled - next;
            if (count <= buffered) { next += (int) count; return count; }
            long skipped = buffered + Math.min(count - buffered, end - position);
            position += skipped - buffered;
            next = filled = 0;
            return skipped;
        }

        @Override public int available() { return (int) Math.min(Integer.MAX_VALUE, (filled - next) + (end - position)); }
    }
}
