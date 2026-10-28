"""Image plumbing around the upscaling nets that needs no torch (numpy, and Pillow for ``resize``), so the pack
step and the tests can use it on machines without the models.

- ``tile_spans`` / ``tile_weights``: how a large input is cut into overlapping tiles that fit in memory, and
  how the tiles' results are cross-faded where they overlap.
- ``colour_fix``: the nets' colour and brightness drift taken back out, region by region, so an upscaled
  texture keeps the original's look.
- ``wrap_pad`` / ``unwrap``: textures the game wraps (road, brick, grass; the sky panorama around the horizon:
  the TilableUV flag in each texture record says along which axes) are upscaled with their own continuation
  past each wrapping edge, then made exactly periodic, so the result tiles without a seam; other edges see
  their mirror image.

The 23-block nets (x4plus, x2plus) look far: an output pixel still moves by a few levels when context 16 to 32
input pixels away changes. So a hard cut between tiles, or between a texture's last column and its first,
shows as a line however wide the margin; a cross-fade between two views of the same pixels does not.
"""
from __future__ import annotations

import math

import numpy as np

# Input pixels of the texture's own continuation shown past each wrapping edge.
WRAP_MARGIN = 16
# Input pixels over which each wrapping edge is cross-faded with the view from across it.
WRAP_BAND = 8
# Overlap between neighbouring tiles of one large input (cross-faded over twice this).
TILE_OVERLAP = 16


def tile_spans(size: int, tile: int) -> list:
    """[(start, end)] covering 0..size in near-equal tiles of at most about ``tile`` pixels; a remainder of up
    to a quarter tile joins the last tile instead of becoming a sliver of its own (wrap margins make 160- and
    288-pixel inputs common, and a sliver costs nearly a whole tile in overlap)."""
    if size <= tile + tile // 4:
        return [(0, size)]
    count = math.ceil(size / tile)
    bounds = [round(i * size / count) for i in range(count + 1)]
    return list(zip(bounds[:-1], bounds[1:]))


def tile_weights(start: int, end: int, core_start: int, core_end: int, scale: int) -> np.ndarray:
    """Weights along one axis for a tile's output (input span start..end, its own share core_start..core_end):
    1 over the share, falling linearly to near 0 across the overlap on each side that has a neighbour. Summed
    over the tiles they cross-fade each overlap; every output pixel keeps weight 1 from the tile it belongs to."""
    n = (end - start) * scale
    weights = np.ones(n, np.float32)
    before = (core_start - start) * scale
    if before:
        weights[:before] = (np.arange(before, dtype=np.float32) + 0.5) / before
    after = (end - core_end) * scale
    if after:
        weights[n - after:] = (np.arange(after, 0, -1, dtype=np.float32) - 0.5) / after
    return weights


def wrap_pad(array: np.ndarray, wrap: str | None, margin: int | None = None):
    """(padded array, rows added above, columns added left) for an HxWxC array. ``wrap`` None: unpadded (the
    nets' own zero padding at the edges, as the interface and car packs were made); otherwise a margin on every
    side, the texture's own continuation along the axes named ("x": across the left and right edges, "y":
    across the top and bottom) and its mirror image along the others. Margins wider than the texture repeat it."""
    if wrap is None:
        return array, 0, 0
    margin = WRAP_MARGIN if margin is None else margin
    padded = np.pad(array, ((0, 0), (margin, margin), (0, 0)), mode="wrap" if "x" in wrap else "reflect")
    padded = np.pad(padded, ((margin, margin), (0, 0), (0, 0)), mode="wrap" if "y" in wrap else "reflect")
    return padded, margin, margin


def _seamless(array: np.ndarray, margin: int, size: int, band: int, axis: int) -> np.ndarray:
    """The ``size`` pixels after ``margin`` along ``axis``, each end cross-faded over ``band`` pixels with the
    net's view of the same pixels from across the opposite end (the margins hold the texture's continuation):
    50/50 at the end pixels, so the last pixel meets the first as two neighbours of one image do."""
    a = np.moveaxis(array, axis, 0)
    core = a[margin:margin + size].astype(np.float32)
    band = min(band, margin, size // 2)
    if band:
        t = (np.arange(band, dtype=np.float32) / band * 0.5 + 0.5).reshape((band,) + (1,) * (a.ndim - 1))
        ahead = a[margin + size:margin + size + band].astype(np.float32)       # core[0:band], seen past the end
        behind = a[margin - band:margin].astype(np.float32)[::-1]              # core[-1:-band-1:-1], seen before the start
        core[:band] = t * core[:band] + (1 - t) * ahead
        core[size - band:] = (t * core[::-1][:band] + (1 - t) * behind)[::-1]
    return np.moveaxis(core, 0, axis)


def unwrap(array: np.ndarray, my: int, mx: int, height: int, width: int, scale: int, wrap: str | None,
           band: int | None = None) -> np.ndarray:
    """The upscaled texture inside an upscaled ``wrap_pad`` result (uint8), periodic along each wrapping axis
    (``wrap`` None: the array as it is)."""
    if wrap is None:
        return array
    band = (WRAP_BAND if band is None else band) * scale
    out = array.astype(np.float32)
    if "x" in wrap:
        out = _seamless(out, mx * scale, width * scale, band, axis=1)
    else:
        out = out[:, mx * scale:(mx + width) * scale]
    if "y" in wrap:
        out = _seamless(out, my * scale, height * scale, band, axis=0)
    else:
        out = out[my * scale:(my + height) * scale]
    return np.clip(np.rint(out), 0, 255).astype(np.uint8)


def resize(image, size, wrap: str | None):
    """A PIL image resized to ``size`` with LANCZOS; along the axes in ``wrap`` the filter reads past each edge
    into the texture's continuation (the image is tiled three times along them, resized, and the middle copy
    kept), so a tiling texture's smaller sizes (mip levels, a 4x net's result taken to 2x) tile too."""
    from PIL import Image
    if not wrap:
        return image.resize(size, Image.LANCZOS)
    nx, ny = (3 if "x" in wrap else 1), (3 if "y" in wrap else 1)
    tiled = Image.new(image.mode, (image.width * nx, image.height * ny))
    for i in range(nx):
        for j in range(ny):
            tiled.paste(image, (i * image.width, j * image.height))
    small = tiled.resize((size[0] * nx, size[1] * ny), Image.LANCZOS)
    left, top = (size[0] if nx == 3 else 0), (size[1] if ny == 3 else 0)
    return small.crop((left, top, left + size[0], top + size[1]))


def _pad_axis(a: np.ndarray, axis: int, width: int, mode: str) -> np.ndarray:
    pads = [(0, 0)] * a.ndim
    pads[axis] = (width, width)
    return np.pad(a, pads, mode=mode)


def _blur(a: np.ndarray, sigma: float, wrap: str | None) -> np.ndarray:
    """Separable Gaussian over the first two axes; across the edges the game wraps it reads the texture's
    continuation, elsewhere its mirror image."""
    radius = max(1, int(np.ceil(3 * sigma)))
    kernel = np.exp(-0.5 * (np.arange(-radius, radius + 1) / sigma) ** 2)
    kernel /= kernel.sum()
    for axis, name in ((0, "y"), (1, "x")):
        padded = _pad_axis(a, axis, radius, "wrap" if wrap and name in wrap else "reflect")
        a = sum(weight * np.take(padded, np.arange(k, k + a.shape[axis]), axis=axis) for k, weight in enumerate(kernel))
    return a


def _upsample(a: np.ndarray, scale: int, wrap: str | None) -> np.ndarray:
    """Bilinear upsampling of the first two axes by ``scale`` (pixel centres aligned)."""
    for axis, name in ((0, "y"), (1, "x")):
        n = a.shape[axis]
        padded = _pad_axis(a, axis, 1, "wrap" if wrap and name in wrap else "edge")
        position = (np.arange(n * scale) + 0.5) / scale + 0.5          # in padded coordinates
        first = np.floor(position).astype(int)
        shape = [1] * a.ndim
        shape[axis] = -1
        t = (position - first).reshape(shape)
        a = np.take(padded, first, axis=axis) * (1 - t) + np.take(padded, first + 1, axis=axis) * t
    return a


def colour_fix(up: np.ndarray, source: np.ndarray, scale: int, wrap: str | None, sigma: float = 1.0) -> np.ndarray:
    """``up`` (uint8, H*scale x W*scale x C) with its colour and brightness brought back, region by region, to
    those of ``source`` (H x W x C): the difference between the source and ``up`` averaged down to the source's
    size, smoothed over about ``sigma`` source pixels and spread back up, is added to ``up``. The nets' detail
    stays, their drift goes (x4plus and x2plus darken busy photographs by a few levels on average, some by
    ten). Smoothing reads across the edges the game wraps, so a tiling result stays tiling."""
    height, width = source.shape[:2]
    out = up.astype(np.float32)
    down = out.reshape(height, scale, width, scale, -1).mean(axis=(1, 3))
    difference = _upsample(_blur(source.astype(np.float32) - down, sigma, wrap), scale, wrap)
    return np.clip(np.rint(out + difference), 0, 255).astype(np.uint8)
