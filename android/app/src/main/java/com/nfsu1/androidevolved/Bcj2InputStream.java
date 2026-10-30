package com.nfsu1.androidevolved;

import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;

/**
 * 7-Zip's BCJ2 filter (method 0303011B), the decoder side: x86 code with its CALL (E8), JMP (E9) and Jcc (0F 8x)
 * targets moved out into streams of their own so they compress better. 7-Zip puts executables in a BCJ2 block of
 * their own - Speed.exe among them - so a game 7z cannot be read without it (Apache Commons Compress, for one, has no
 * BCJ2).
 *
 * Four inputs: the main stream (everything but the converted targets), the CALL targets, the JMP/Jcc targets (both
 * big-endian absolute addresses), and a range-coded stream of one bit per candidate opcode saying whether its target
 * was moved out. A moved-out target is turned back into the relative form x86 uses: absolute - (position after the
 * four bytes), modulo 2^32. The bit's probability is chosen by the opcode: E8 by the byte before it (256 contexts),
 * E9 one, Jcc one; 11-bit probabilities, shift-5 adaptation, the LZMA range coder.
 *
 * This is the public-domain LZMA SDK's Bcj2_Decode (Igor Pavlov) as a stream; it normalises the range before each bit
 * rather than after, which reads the same bits and does not ask for a byte past the end of the range coder's stream.
 */
final class Bcj2InputStream extends InputStream {
    private static final int TOP = 1 << 24, MODEL_BITS = 11, MODEL_TOTAL = 1 << MODEL_BITS, MOVE_BITS = 5;

    private final Input main, call, jump, rc;
    private final long size;
    private long position;                              // output bytes so far
    private final short[] probabilities = new short[258]; // [0, 256): E8 after that byte; 256: E9; 257: Jcc
    private int range = 0xFFFFFFFF, code;
    private boolean started;
    private int previous;                               // the byte before the next main-stream byte
    private final byte[] operand = new byte[4];
    private int operandNext = 4;                        // operand[operandNext, 4) still to be handed out

    Bcj2InputStream(InputStream main, InputStream call, InputStream jump, InputStream rangeCoded, long size) {
        this.main = new Input(main, 1 << 16, "main");
        this.call = new Input(call, 1 << 12, "call");
        this.jump = new Input(jump, 1 << 12, "jump");
        this.rc = new Input(rangeCoded, 1 << 12, "range coder");
        this.size = size;
        for (int i = 0; i < probabilities.length; i++) probabilities[i] = MODEL_TOTAL >> 1;
    }

    static boolean isJump(int previous, int b) { return (b & 0xFE) == 0xE8 || (previous == 0x0F && (b & 0xF0) == 0x80); }

    @Override public int read() throws IOException {
        byte[] one = new byte[1];
        return read(one, 0, 1) == 1 ? one[0] & 0xFF : -1;
    }

    @Override public int read(byte[] buffer, int offset, int length) throws IOException {
        if (length == 0) return 0;
        if (position >= size) return -1;
        if (!started) {
            for (int i = 0; i < 5; i++) code = code << 8 | rc.next();
            started = true;
        }
        int n = 0;
        while (n < length && position < size) {
            if (operandNext < 4) {
                buffer[offset + n++] = operand[operandNext++];
                position++;
                continue;
            }
            int b = main.next();
            buffer[offset + n++] = (byte) b;
            position++;
            if (!isJump(previous, b) || position == size) { previous = b; continue; }   // an opcode last gets no bit
            if (bit(b == 0xE8 ? previous : b == 0xE9 ? 256 : 257) == 0) { previous = b; continue; }
            Input targets = b == 0xE8 ? call : jump;
            int absolute = targets.next() << 24 | targets.next() << 16 | targets.next() << 8 | targets.next();
            int relative = absolute - (int) (position + 4);
            operand[0] = (byte) relative;
            operand[1] = (byte) (relative >>> 8);
            operand[2] = (byte) (relative >>> 16);
            operand[3] = (byte) (relative >>> 24);
            operandNext = 0;
            previous = (relative >>> 24) & 0xFF;
        }
        return n;
    }

    private int bit(int index) throws IOException {
        if (Integer.compareUnsigned(range, TOP) < 0) {
            range <<= 8;
            code = code << 8 | rc.next();
        }
        int probability = probabilities[index];
        int bound = (range >>> MODEL_BITS) * probability;
        if (Integer.compareUnsigned(code, bound) < 0) {
            range = bound;
            probabilities[index] = (short) (probability + ((MODEL_TOTAL - probability) >>> MOVE_BITS));
            return 0;
        }
        range -= bound;
        code -= bound;
        probabilities[index] = (short) (probability - (probability >>> MOVE_BITS));
        return 1;
    }

    @Override public void close() throws IOException {
        IOException first = null;
        for (Input input : new Input[] {main, call, jump, rc}) {
            try { input.in.close(); } catch (IOException error) { if (first == null) first = error; }
        }
        if (first != null) throw first;
    }

    /** One input, buffered; ending early is an error (the filter knows how much it needs from each). */
    private static final class Input {
        final InputStream in;
        final byte[] buffer;
        final String name;
        int next, filled;

        Input(InputStream in, int size, String name) { this.in = in; this.buffer = new byte[size]; this.name = name; }

        int next() throws IOException {
            if (next == filled) {
                int n = in.read(buffer, 0, buffer.length);
                if (n <= 0) throw new EOFException("BCJ2 " + name + " stream ends early");
                next = 0;
                filled = n;
            }
            return buffer[next++] & 0xFF;
        }
    }
}
