"""nfstex.dxt against a straightforward reference decoder."""
import unittest

import numpy as np

from nfstex import dxt


def decode_color_block(block: bytes, four_color: bool) -> np.ndarray:
    c0 = block[0] | (block[1] << 8)
    c1 = block[2] | (block[3] << 8)

    def expand(c):
        r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
        return np.array([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], float)
    p0, p1 = expand(c0), expand(c1)
    if four_color or c0 > c1:
        palette = [p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3]
    else:
        palette = [p0, p1, (p0 + p1) / 2, np.zeros(3)]
    bits = block[4] | (block[5] << 8) | (block[6] << 16) | (block[7] << 24)
    return np.array([palette[(bits >> (2 * i)) & 3] for i in range(16)])


def decode(data: bytes, width: int, height: int, dxt3: bool) -> np.ndarray:
    out = np.zeros((height, width, 4), float)
    size = 16 if dxt3 else 8
    for by in range(height // 4):
        for bx in range(width // 4):
            block = data[(by * (width // 4) + bx) * size:][:size]
            colors = decode_color_block(block[8:] if dxt3 else block, dxt3)
            for i in range(16):
                y, x = by * 4 + i // 4, bx * 4 + i % 4
                out[y, x, :3] = colors[i]
                if dxt3:
                    nibble = (block[i // 2] >> (4 * (i % 2))) & 15
                    out[y, x, 3] = nibble * 17
                else:
                    out[y, x, 3] = 255
    return out


class DxtTests(unittest.TestCase):
    def test_dxt3_alpha_nibbles_and_colour(self):
        rng = np.random.default_rng(1)
        image = np.zeros((8, 8, 4), np.uint8)
        image[..., :3] = rng.integers(0, 256, (8, 8, 3))
        image[..., 3] = np.repeat(np.arange(0, 256, 32).reshape(1, 8), 8, axis=0)   # alpha varies per column
        data = dxt.encode_dxt3(image)
        self.assertEqual(len(data), 4 * 16)
        back = decode(data, 8, 8, True)
        alpha_error = np.abs(back[..., 3] - image[..., 3]).max()
        self.assertLessEqual(alpha_error, 17, "4-bit alpha within one step, in pixel order (no striping)")
        expected = [int((v * 15 + 127) // 255) * 17 for v in range(0, 256, 32)]
        self.assertEqual([int(v) for v in back[0, :, 3]], expected)
        colour_error = np.abs(back[..., :3] - image[..., :3]).mean()
        self.assertLess(colour_error, 64, "random noise still lands within the block palette (a stress case)")

    def test_gradient_fidelity(self):
        x = np.linspace(0, 255, 64)
        image = np.zeros((16, 64, 4), np.uint8)
        image[..., 0] = x; image[..., 1] = 255 - x; image[..., 2] = 128; image[..., 3] = 255
        for dxt3 in (False, True):
            data = dxt.encode_dxt3(image) if dxt3 else dxt.encode_dxt1(image)
            back = decode(data, 64, 16, dxt3)
            mse = ((back[..., :3] - image[..., :3].astype(float)) ** 2).mean()
            psnr = 10 * np.log10(255 * 255 / max(mse, 1e-9))
            self.assertGreater(psnr, 38, f"gradient PSNR {psnr:.1f} dB (dxt3={dxt3})")

    def test_flat_block_and_padding(self):
        image = np.full((6, 10, 4), 200, np.uint8)      # not multiples of 4: padded with the edge
        data = dxt.encode_dxt1(image)
        self.assertEqual(len(data), 2 * 3 * 8)
        back = decode(data, 12, 8, False)
        self.assertTrue(np.all(np.abs(back[:6, :10, :3] - 200) <= 4))


if __name__ == "__main__":
    unittest.main()
