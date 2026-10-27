package com.nfsu1.androidevolved;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.security.MessageDigest;
import java.util.Arrays;
import org.junit.Test;

/** The DXVK state cache format (version 10, dxvk_state_cache.cpp) parsed, merged and written back. */
public class ShaderCacheSeedTest {
    private static final int VERSION = 10;

    private static byte[] entry(int stageMask, byte[] data) throws Exception {
        byte[] hash = MessageDigest.getInstance("SHA-1").digest(data);
        ByteBuffer out = ByteBuffer.allocate(4 + 20 + data.length).order(ByteOrder.LITTLE_ENDIAN);
        out.putInt((data.length << 8) | (stageMask & 0xff)).put(hash).put(data);
        return out.array();
    }

    private static byte[] cache(int version, byte[]... entries) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        out.write('D'); out.write('X'); out.write('V'); out.write('K');
        ByteBuffer header = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putInt(version).putInt(0);
        out.write(header.array(), 0, 8);
        for (byte[] e : entries) out.write(e, 0, e.length);
        return out.toByteArray();
    }

    private static byte[] data(int seed, int length) {
        byte[] d = new byte[length];
        for (int i = 0; i < length; i++) d[i] = (byte) (seed * 31 + i);
        return d;
    }

    @Test public void parsesTheRealPixelCache() throws Exception {
        byte[] real;
        try (InputStream in = getClass().getClassLoader().getResourceAsStream("pixel10-session1.dxvk-cache")) {
            assertNotNull("test resource", in);
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[8192];
            for (int n; (n = in.read(buffer)) != -1;) out.write(buffer, 0, n);
            real = out.toByteArray();
        }
        ShaderCacheSeed.StateCache cache = ShaderCacheSeed.StateCache.parse(real);
        assertNotNull(cache);
        assertEquals(10, cache.version);
        assertEquals(23, cache.entries.size());        // the first Pixel 10 Pro XL session: front end only
        assertArrayEquals("round trip is byte-identical", real, cache.toBytes());
    }

    @Test public void mergeIsAUnionKeyedByHash() throws Exception {
        byte[] a = entry(0x11, data(1, 203)), b = entry(0x11, data(2, 207)), c = entry(0x11, data(3, 203));
        byte[] device = cache(VERSION, a, b), bundled = cache(VERSION, b, c);
        byte[] merged = ShaderCacheSeed.mergedStateCache(device, bundled);
        assertNotNull(merged);
        ShaderCacheSeed.StateCache result = ShaderCacheSeed.StateCache.parse(merged);
        assertEquals(3, result.entries.size());
        // The device's own entries keep their order; the new one follows.
        assertArrayEquals(a, result.entries.get(0));
        assertArrayEquals(b, result.entries.get(1));
        assertArrayEquals(c, result.entries.get(2));
        assertNull("nothing new: no rewrite", ShaderCacheSeed.mergedStateCache(merged, bundled));
    }

    @Test public void seedReplacesAMissingOrForeignFileButNotAnotherVersion() throws Exception {
        byte[] bundled = cache(VERSION, entry(0x11, data(7, 203)));
        assertArrayEquals(bundled, ShaderCacheSeed.mergedStateCache(null, bundled));
        assertArrayEquals(bundled, ShaderCacheSeed.mergedStateCache("not a cache".getBytes(), bundled));
        assertNull(ShaderCacheSeed.mergedStateCache(cache(9, entry(0x11, data(8, 203))), bundled));
        assertNull("an empty seed changes nothing", ShaderCacheSeed.mergedStateCache(cache(VERSION), cache(VERSION)));
    }

    @Test public void truncatedAndCorruptEntriesStopTheParseLikeDxvk() throws Exception {
        byte[] good = entry(0x11, data(1, 203)), second = entry(0x11, data(2, 203));
        byte[] whole = cache(VERSION, good, second);
        byte[] truncated = Arrays.copyOf(whole, whole.length - 10);
        assertEquals(1, ShaderCacheSeed.StateCache.parse(truncated).entries.size());
        byte[] corrupt = whole.clone();
        corrupt[12 + 4 + 20 + 5] ^= 0x55;            // flip a data byte of the first entry: hash mismatch
        assertEquals(0, ShaderCacheSeed.StateCache.parse(corrupt).entries.size());
        // A corrupt device file is still merged from (what it lacks is appended after its valid prefix).
        byte[] merged = ShaderCacheSeed.mergedStateCache(truncated, cache(VERSION, second));
        assertEquals(2, ShaderCacheSeed.StateCache.parse(merged).entries.size());
        assertTrue(ShaderCacheSeed.StateCache.parse(merged).hashes.size() == 2);
    }
}
