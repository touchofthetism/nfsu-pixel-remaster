package com.nfsu1.androidevolved;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import org.junit.Test;

/** Mod packs: entry rules, stacking installs, reverse-order removal, case-insensitive targets. */
public class ModPacksTest {
    private static byte[] zip(String... nameAndContent) throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        try (ZipOutputStream z = new ZipOutputStream(out)) {
            for (int i = 0; i < nameAndContent.length; i += 2) {
                z.putNextEntry(new ZipEntry(nameAndContent[i]));
                z.write(nameAndContent[i + 1].getBytes(StandardCharsets.UTF_8));
                z.closeEntry();
            }
        }
        return out.toByteArray();
    }

    private static String read(File file) throws Exception { return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8); }

    private static File gameRoot() throws Exception {
        File root = Files.createTempDirectory("nfsu1").toFile();
        assertTrue(new File(root, "Global").mkdirs());
        assertTrue(new File(root, "Tracks").mkdirs());
        Files.write(new File(root, "Global/GLOBALB.BUN").toPath(), "stock-globalb".getBytes(StandardCharsets.UTF_8));
        Files.write(new File(root, "Global/GlobalB.lzc").toPath(), "stock-lzc".getBytes(StandardCharsets.UTF_8));
        Files.write(new File(root, "Global/HUDTEX.BIN").toPath(), "stock-hud".getBytes(StandardCharsets.UTF_8));
        Files.write(new File(root, "Speed.exe").toPath(), "exe".getBytes(StandardCharsets.UTF_8));
        return root;
    }

    @Test public void entryRules() {
        assertEquals("Global/GLOBALB.BUN", ModPacks.entryName("Global/GLOBALB.BUN"));
        assertEquals("Tracks/TRACKB1000.lzc", ModPacks.entryName("NFSU Free Roam Beta 2.0/Tracks/TRACKB1000.lzc"));
        assertEquals("case is kept; the install step resolves it", "global/x.bin", ModPacks.entryName("global\\x.bin"));
        assertNull("the executable is never replaced", ModPacks.entryName("Speed.exe"));
        assertNull(ModPacks.entryName("NFSU1WrongWayRemover.exe"));
        assertNull(ModPacks.entryName("readme.txt"));
        assertNull(ModPacks.entryName("scripts/mod.asi"));
        assertNull(ModPacks.entryName("Save/PLAYER/career.sav"));
        assertNull(ModPacks.entryName("../Global/x"));
        assertNull(ModPacks.entryName("Global/../x"));
        assertNull(ModPacks.entryName("__MACOSX/Global/._x"));
        assertNull(ModPacks.entryName("Mods/Other/Global/x.bin"));     // two wrapping folders: not a pack layout
        assertEquals("hd-interface-pack", ModPacks.slug("HD interface pack!"));
        assertEquals("Free Roam Beta 2", ModPacks.cleanName("Free_Roam_Beta_2.zip"));
    }

    @Test public void installStacksAndUninstallRestoresInReverse() throws Exception {
        File root = gameRoot();
        byte[] free = zip("Free Roam/Global/GLOBALB.BUN", "freeroam-globalb", "Free Roam/Global/GlobalB.lzc", "freeroam-lzc",
                "Free Roam/Tracks/TRACKB1000.lzc", "city", "Free Roam/readme.txt", "ignored", "Free Roam/Remover.exe", "ignored");
        ModPacks.Pack a = ModPacks.install(new ByteArrayInputStream(free), root, "Free_Roam_Beta_2.zip", null);
        assertEquals("Free Roam Beta 2", a.name);
        assertEquals(3, a.files.size());
        assertEquals(2, a.replaced.size());
        assertEquals("freeroam-globalb", read(new File(root, "Global/GLOBALB.BUN")));
        assertEquals("city", read(new File(root, "Tracks/TRACKB1000.lzc")));
        assertEquals("stock-globalb", read(new File(root, ModPacks.STORE + "/" + a.id + "/Global/GLOBALB.BUN")));
        // A second pack with a manifest, touching one of the same files, in different case.
        byte[] hd = zip("nfsu-modpack.json", "{\"name\":\"HD pack\",\"version\":\"3\",\"notes\":\"gauges\"}",
                "global/globalb.bun", "hd-globalb", "Global/HUDTEX.BIN", "hd-hud");
        ModPacks.Pack b = ModPacks.install(new ByteArrayInputStream(hd), root, "x.zip", null);
        assertEquals("HD pack", b.name);
        assertEquals("3", b.version);
        assertEquals("hd-globalb", read(new File(root, "Global/GLOBALB.BUN")));
        assertFalse("case-insensitive target: no duplicate file", new File(root, "global/globalb.bun").getCanonicalFile().getName().equals("globalb.bun") && new File(root, "Global").list().length != 3);
        assertEquals("freeroam-globalb", read(new File(root, ModPacks.STORE + "/" + b.id + "/Global/GLOBALB.BUN")));
        List<ModPacks.Pack> installed = ModPacks.installed(root);
        assertEquals(2, installed.size());
        assertEquals(a.id, installed.get(0).id);
        // Removing the first pack removes the second as well, newest first, and restores stock.
        List<ModPacks.Pack> removed = ModPacks.uninstall(root, a.id);
        assertEquals(2, removed.size());
        assertEquals(b.id, removed.get(0).id);
        assertEquals("stock-globalb", read(new File(root, "Global/GLOBALB.BUN")));
        assertEquals("stock-lzc", read(new File(root, "Global/GlobalB.lzc")));
        assertEquals("stock-hud", read(new File(root, "Global/HUDTEX.BIN")));
        assertFalse("a file the pack added is gone", new File(root, "Tracks/TRACKB1000.lzc").exists());
        assertTrue(ModPacks.installed(root).isEmpty());
        assertEquals("exe", read(new File(root, "Speed.exe")));
        String[] leftovers = new File(root, ModPacks.STORE).list((dir, name) -> !name.equals(ModPacks.REGISTRY));
        assertEquals(0, leftovers == null ? 0 : leftovers.length);
    }

    @Test public void refusesZipsWithoutGameFiles() throws Exception {
        File root = gameRoot();
        try {
            ModPacks.install(new ByteArrayInputStream(zip("readme.txt", "hi", "Speed.exe", "cracked")), root, "junk.zip", null);
            throw new AssertionError("accepted a ZIP without game files");
        } catch (java.io.IOException expected) { }
        assertEquals("exe", read(new File(root, "Speed.exe")));
        assertTrue(ModPacks.installed(root).isEmpty());
    }
}
