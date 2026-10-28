"""Tiling and seamless-wrap plumbing (tools/nfstex/tiling.py): numpy only, no nets."""
import importlib.util
import unittest

HAVE_NUMPY = importlib.util.find_spec("numpy") is not None


def box_net(array, scale: int, radius: int):
    """A stand-in for an upscaling net: nearest-neighbour upscale, then a box blur of ``radius`` output pixels
    that, like the real nets' convolutions, sees black past the array's edge (its output depends on the margin
    it is given whenever the radius reaches past it)."""
    import numpy as np
    image = np.repeat(np.repeat(array.astype(np.float32), scale, axis=0), scale, axis=1)
    padded = np.pad(image, ((radius, radius), (radius, radius), (0, 0)))
    k = 2 * radius + 1
    return sum(padded[dy:dy + image.shape[0], dx:dx + image.shape[1]] for dy in range(k) for dx in range(k)) / (k * k)


@unittest.skipUnless(HAVE_NUMPY, "tiling needs numpy")
class TilingTests(unittest.TestCase):
    def setUp(self):
        import numpy as np
        from nfstex import tiling
        self.np, self.tiling = np, tiling
        rng = np.random.default_rng(3)
        self.texture = rng.integers(0, 256, size=(24, 40, 3)).astype(np.uint8)

    def test_spans_cover_without_slivers(self):
        spans = self.tiling.tile_spans
        self.assertEqual([(0, 160)], spans(160, 128))              # up to a quarter tile more stays one tile
        self.assertEqual([(0, 181), (181, 363), (363, 544)], spans(544, 256))
        for size in range(1, 1300, 7):
            parts = spans(size, 128)
            self.assertEqual(0, parts[0][0])
            self.assertEqual(size, parts[-1][1])
            self.assertTrue(all(a[1] == b[0] for a, b in zip(parts, parts[1:])))
            self.assertTrue(all(end - start <= 160 for start, end in parts))

    def test_tile_weights_cross_fade_to_one(self):
        np = self.np
        size, tile, pad, scale = 700, 128, 16, 2
        total = np.zeros(size * scale, np.float32)
        for start, end in self.tiling.tile_spans(size, tile):
            first, last = max(0, start - pad), min(size, end + pad)
            weights = self.tiling.tile_weights(first, last, start, end, scale)
            self.assertEqual((last - first) * scale, len(weights))
            self.assertTrue(np.all(weights[(start - first) * scale:(end - first) * scale] == 1))
            self.assertTrue(np.all(weights > 0))
            total[first * scale:last * scale] += weights
        self.assertTrue(np.all(total >= 1))           # every pixel keeps its own tile's full weight
        self.assertTrue(np.all(total <= 2))

    def test_pad_wraps_the_named_axes_and_mirrors_the_rest(self):
        np, pad = self.np, self.tiling.wrap_pad
        t = self.texture
        same, my, mx = pad(t, None)
        self.assertIs(t, same)
        self.assertEqual((0, 0), (my, mx))
        padded, my, mx = pad(t, "x", 5)
        self.assertEqual((5, 5), (my, mx))
        self.assertEqual((24 + 10, 40 + 10, 3), padded.shape)
        np.testing.assert_array_equal(t, padded[5:-5, 5:-5])
        np.testing.assert_array_equal(t[:, -5:], padded[5:-5, :5])           # wrapped across the left edge
        np.testing.assert_array_equal(t[1:6][::-1], padded[:5, 5:-5])        # mirrored above the top edge
        padded, _, _ = pad(t, "xy", 30)                                       # wider than the texture: repeats
        np.testing.assert_array_equal(t[-6:], padded[:6, 30:-30])

    def test_unwrap_gives_the_periodic_result(self):
        np, tiling = self.np, self.tiling
        t, scale = self.texture, 2
        h, w = t.shape[:2]
        # A net that sees no farther than the margin: exactly the result of upscaling the endless tiling.
        endless = box_net(np.tile(t, (3, 3, 1)), scale, 2)[h * scale:2 * h * scale, w * scale:2 * w * scale]
        padded, my, mx = tiling.wrap_pad(t, "xy", 6)
        out = tiling.unwrap(box_net(padded, scale, 2), my, mx, h, w, scale, "xy", band=3)
        self.assertEqual((h * scale, w * scale, 3), out.shape)
        self.assertEqual(np.uint8, out.dtype)
        np.testing.assert_array_equal(np.rint(endless), out)
        # One axis wrapped, the other mirrored: periodic along x only, the y edges as a mirrored copy gives them.
        padded, my, mx = tiling.wrap_pad(t, "x", 6)
        out = tiling.unwrap(box_net(padded, scale, 2), my, mx, h, w, scale, "x", band=3)
        mirrored = np.concatenate([t[6:0:-1], t, t[-2:-8:-1]])
        expected = box_net(np.tile(mirrored, (1, 3, 1)), scale, 2)[6 * scale:(6 + h) * scale, w * scale:2 * w * scale]
        np.testing.assert_array_equal(np.rint(expected), out)
        # wrap None: the array as it came.
        raw = box_net(t, scale, 2)
        self.assertIs(raw, tiling.unwrap(raw, 0, 0, h, w, scale, None))

    def test_the_cross_fade_closes_the_seam_a_far_seeing_net_leaves(self):
        np, tiling = self.np, self.tiling
        t, scale = self.texture, 2
        padded, my, mx = tiling.wrap_pad(t, "xy", 2)
        seams = {}
        for band in (0, 1):
            out = tiling.unwrap(box_net(padded, scale, 6), my, mx, t.shape[0], t.shape[1], scale, "xy", band=band)
            out = out.astype(np.float32)
            seams[band] = (np.abs(out[:, 0] - out[:, -1]).mean(), np.abs(np.diff(out, axis=1)).mean())
        hard, faded = seams[0], seams[1]
        self.assertGreater(hard[0], 1.5 * hard[1])            # a hard cut: the wrap jump stands out
        self.assertLess(faded[0], faded[1])                    # cross-faded: no bigger than any other step

    def test_colour_fix_takes_drift_out_and_keeps_detail(self):
        np, tiling = self.np, self.tiling
        t, scale = self.texture, 4
        h, w = t.shape[:2]
        # A "net" that adds fine detail (a +-20 checker inside each upscaled pixel) and drifts darker on the left.
        base = np.repeat(np.repeat(t.astype(np.float32), scale, axis=0), scale, axis=1)
        checker = np.indices((h * scale, w * scale)).sum(axis=0) % 2 * 40 - 20
        drift = np.linspace(-14, 0, w * scale)[None, :, None]
        up = np.clip(base + checker[..., None] + drift, 0, 255).astype(np.uint8)
        fixed = tiling.colour_fix(up, t, scale, "xy")
        def down(a):
            return a.astype(np.float32).reshape(h, scale, w, scale, 3).mean(axis=(1, 3))
        self.assertGreater(np.abs(down(up) - t).mean(), 4)
        self.assertLess(np.abs(down(fixed) - t).mean(), 2)                    # region by region back to the source
        detail = lambda a: np.abs(np.diff(a.astype(np.float32), axis=1)).mean()
        self.assertGreater(detail(fixed), 0.9 * detail(up))                   # the detail stays
        # Smoothing reads across wrapped edges: the correction of a texture that tiles tiles too.
        source = (t // 2 + 64).astype(np.uint8)
        x = np.arange(w * scale)
        wave = (10 + 8 * np.sin(2 * np.pi * (x + 0.5) / (w * scale)))[None, :, None]
        drifted = (np.repeat(np.repeat(source, scale, axis=0), scale, axis=1) + wave).astype(np.uint8)
        correction = tiling.colour_fix(drifted, source, scale, "xy").astype(np.float32) - drifted
        self.assertLess(np.abs(correction[:, 0] - correction[:, -1]).mean(), 1.5)     # no step at the wrap
        self.assertAlmostEqual(-10, correction.mean(), delta=0.5)
        np.testing.assert_allclose(-wave[0, :, 0], correction.mean(axis=(0, 2)), atol=1.5)


if __name__ == "__main__":
    unittest.main()
