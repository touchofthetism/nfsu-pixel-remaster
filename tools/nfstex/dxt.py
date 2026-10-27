"""DXT1 / DXT3 block compression in numpy (Pillow 12's DXT3 encoder writes a striped alpha channel).

Range fit: for each 4x4 block the two colour endpoints are the block's extreme colours along its
principal luminance axis, refined by one least-squares pass; every pixel takes the nearest of the four
palette entries. DXT3 alpha is the explicit 4-bit quantisation. Good for interface art and photographs
alike; nothing adaptive, so a whole 512x512 image encodes in well under a second.
"""
from __future__ import annotations

import numpy as np


def _to_565(rgb: np.ndarray) -> np.ndarray:
    r = (rgb[..., 0].astype(np.uint32) * 31 + 127) // 255
    g = (rgb[..., 1].astype(np.uint32) * 63 + 127) // 255
    b = (rgb[..., 2].astype(np.uint32) * 31 + 127) // 255
    return (r << 11) | (g << 5) | b


def _from_565(c: np.ndarray) -> np.ndarray:
    r = (c >> 11) & 31
    g = (c >> 5) & 63
    b = c & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], axis=-1).astype(np.float32)


def _blocks(image: np.ndarray) -> np.ndarray:
    """HxWxC (H, W multiples of 4) -> (H/4*W/4, 16, C) in row-major pixel order."""
    h, w, c = image.shape
    return image.reshape(h // 4, 4, w // 4, 4, c).transpose(0, 2, 1, 3, 4).reshape(-1, 16, c)


def _pad(image: np.ndarray) -> np.ndarray:
    h, w = image.shape[:2]
    ph, pw = (-h) % 4, (-w) % 4
    if ph or pw:
        image = np.pad(image, ((0, ph), (0, pw), (0, 0)), mode="edge")
    return image


def encode_color_blocks(rgb: np.ndarray, four_color: bool) -> np.ndarray:
    """(N,16,3) uint8 -> (N,8) uint8 DXT1 colour blocks. four_color forces color0 > color1 (DXT3/5)."""
    pixels = rgb.astype(np.float32)
    n = pixels.shape[0]
    # Principal axis per block: covariance's dominant direction approximated by the colour spread.
    mean = pixels.mean(axis=1, keepdims=True)
    centred = pixels - mean
    cov = np.einsum("nij,nik->njk", centred, centred)
    axis = np.ones((n, 3), np.float32)
    for _ in range(8):   # power iteration
        axis = np.einsum("njk,nk->nj", cov, axis)
        norm = np.linalg.norm(axis, axis=1, keepdims=True)
        axis = np.where(norm > 1e-6, axis / np.maximum(norm, 1e-6), np.array([[0.299, 0.587, 0.114]], np.float32))
    projection = np.einsum("nik,nk->ni", centred, axis)
    lo = pixels[np.arange(n), projection.argmin(axis=1)]
    hi = pixels[np.arange(n), projection.argmax(axis=1)]
    # One least-squares refinement of the endpoints given the nearest-palette assignment.
    for _ in range(2):
        c0, c1 = lo, hi
        palette = np.stack([c0, c1, (2 * c0 + c1) / 3, (c0 + 2 * c1) / 3], axis=1)   # (n,4,3)
        d = ((pixels[:, :, None, :] - palette[:, None, :, :]) ** 2).sum(axis=3)       # (n,16,4)
        idx = d.argmin(axis=2)                                                         # (n,16)
        # Each index i corresponds to weight t_i of c1: 0, 1, 1/3, 2/3.
        t = np.array([0.0, 1.0, 1 / 3, 2 / 3], np.float32)[idx]                       # (n,16)
        a = (1 - t)
        a2, at, t2 = (a * a).sum(1), (a * t).sum(1), (t * t).sum(1)
        det = a2 * t2 - at * at
        sa = np.einsum("ni,nik->nk", a, pixels)
        st = np.einsum("ni,nik->nk", t, pixels)
        ok = det > 1e-3
        new_lo = np.where(ok[:, None], (sa * t2[:, None] - st * at[:, None]) / np.where(ok, det, 1)[:, None], lo)
        new_hi = np.where(ok[:, None], (st * a2[:, None] - sa * at[:, None]) / np.where(ok, det, 1)[:, None], hi)
        lo, hi = np.clip(new_lo, 0, 255), np.clip(new_hi, 0, 255)
    q0, q1 = _to_565(np.rint(lo)), _to_565(np.rint(hi))
    if four_color:
        swap = q0 < q1
        q0, q1 = np.where(swap, q1, q0), np.where(swap, q0, q1)
        equal = q0 == q1
        # Equal endpoints: all four palette entries coincide; any indices work (use 0).
    else:
        swap = q0 < q1
        q0, q1 = np.where(swap, q1, q0), np.where(swap, q0, q1)
        equal = q0 == q1
    p0, p1 = _from_565(q0), _from_565(q1)
    palette = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], axis=1)
    d = ((pixels[:, :, None, :] - palette[:, None, :, :]) ** 2).sum(axis=3)
    idx = d.argmin(axis=2).astype(np.uint32)
    idx[equal] = 0
    bits = np.zeros(n, np.uint32)
    for i in range(16):
        bits |= idx[:, i] << (2 * i)
    out = np.zeros((n, 8), np.uint8)
    out[:, 0] = q0 & 0xFF; out[:, 1] = q0 >> 8
    out[:, 2] = q1 & 0xFF; out[:, 3] = q1 >> 8
    for i in range(4):
        out[:, 4 + i] = (bits >> (8 * i)) & 0xFF
    return out


def encode_dxt1(rgba: np.ndarray) -> bytes:
    """Opaque DXT1 (alpha ignored) of an RGBA/RGB uint8 image."""
    image = _pad(np.ascontiguousarray(rgba[..., :3]))
    return encode_color_blocks(_blocks(image), four_color=False).tobytes()


def encode_dxt3(rgba: np.ndarray) -> bytes:
    """DXT3 of an RGBA uint8 image: explicit 4-bit alpha (low nibble = left pixel) + colour block."""
    image = _pad(np.ascontiguousarray(rgba))
    blocks = _blocks(image)
    alpha = (blocks[:, :, 3].astype(np.uint32) * 15 + 127) // 255           # (n,16) 0..15
    n = blocks.shape[0]
    alpha_bytes = np.zeros((n, 8), np.uint8)
    for i in range(8):
        alpha_bytes[:, i] = (alpha[:, 2 * i] | (alpha[:, 2 * i + 1] << 4)) & 0xFF
    color = encode_color_blocks(blocks[:, :, :3], four_color=True)
    return np.concatenate([alpha_bytes, color], axis=1).tobytes()
