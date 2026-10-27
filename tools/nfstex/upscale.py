"""Texture upscaling with Real-ESRGAN weights, without the basicsr package.

Two network definitions, matching the public Real-ESRGAN checkpoints byte for byte:
  * ``RRDBNet`` — RealESRGAN_x4plus (23 blocks), RealESRGAN_x4plus_anime_6B (6 blocks), RealESRGAN_x2plus
    (23 blocks, pixel-unshuffle input, scale 2).
  * ``SRVGGNetCompact`` — realesr-animevideov3 (x4, tiny; clean edges for UI and gauge art, fast on a CPU).

Alpha is upscaled as a separate greyscale image through the same network (replicated to RGB), which keeps
mask edges as crisp as the colour edges. Images are padded to a multiple of 4 and tiled so large textures
fit in memory. Runs on CPU; torch is the only dependency (plus numpy/Pillow for the image plumbing).
"""
from __future__ import annotations

import functools
import math
import os
from typing import Tuple

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
from PIL import Image


# ---- RRDBNet -----------------------------------------------------------------------------------------------

class ResidualDenseBlock(nn.Module):
    def __init__(self, num_feat=64, num_grow_ch=32):
        super().__init__()
        self.conv1 = nn.Conv2d(num_feat, num_grow_ch, 3, 1, 1)
        self.conv2 = nn.Conv2d(num_feat + num_grow_ch, num_grow_ch, 3, 1, 1)
        self.conv3 = nn.Conv2d(num_feat + 2 * num_grow_ch, num_grow_ch, 3, 1, 1)
        self.conv4 = nn.Conv2d(num_feat + 3 * num_grow_ch, num_grow_ch, 3, 1, 1)
        self.conv5 = nn.Conv2d(num_feat + 4 * num_grow_ch, num_feat, 3, 1, 1)
        self.lrelu = nn.LeakyReLU(negative_slope=0.2, inplace=True)

    def forward(self, x):
        x1 = self.lrelu(self.conv1(x))
        x2 = self.lrelu(self.conv2(torch.cat((x, x1), 1)))
        x3 = self.lrelu(self.conv3(torch.cat((x, x1, x2), 1)))
        x4 = self.lrelu(self.conv4(torch.cat((x, x1, x2, x3), 1)))
        x5 = self.conv5(torch.cat((x, x1, x2, x3, x4), 1))
        return x5 * 0.2 + x


class RRDB(nn.Module):
    def __init__(self, num_feat, num_grow_ch=32):
        super().__init__()
        self.rdb1 = ResidualDenseBlock(num_feat, num_grow_ch)
        self.rdb2 = ResidualDenseBlock(num_feat, num_grow_ch)
        self.rdb3 = ResidualDenseBlock(num_feat, num_grow_ch)

    def forward(self, x):
        out = self.rdb3(self.rdb2(self.rdb1(x)))
        return out * 0.2 + x


def pixel_unshuffle(x, scale):
    b, c, hh, hw = x.size()
    out_channel = c * (scale ** 2)
    h, w = hh // scale, hw // scale
    x_view = x.view(b, c, h, scale, w, scale)
    return x_view.permute(0, 1, 3, 5, 2, 4).reshape(b, out_channel, h, w)


class RRDBNet(nn.Module):
    def __init__(self, num_in_ch=3, num_out_ch=3, scale=4, num_feat=64, num_block=23, num_grow_ch=32):
        super().__init__()
        self.scale = scale
        if scale == 2:
            num_in_ch = num_in_ch * 4
        elif scale == 1:
            num_in_ch = num_in_ch * 16
        self.conv_first = nn.Conv2d(num_in_ch, num_feat, 3, 1, 1)
        self.body = nn.Sequential(*[RRDB(num_feat, num_grow_ch) for _ in range(num_block)])
        self.conv_body = nn.Conv2d(num_feat, num_feat, 3, 1, 1)
        self.conv_up1 = nn.Conv2d(num_feat, num_feat, 3, 1, 1)
        self.conv_up2 = nn.Conv2d(num_feat, num_feat, 3, 1, 1)
        self.conv_hr = nn.Conv2d(num_feat, num_feat, 3, 1, 1)
        self.conv_last = nn.Conv2d(num_feat, num_out_ch, 3, 1, 1)
        self.lrelu = nn.LeakyReLU(negative_slope=0.2, inplace=True)

    def forward(self, x):
        if self.scale == 2:
            feat = pixel_unshuffle(x, scale=2)
        elif self.scale == 1:
            feat = pixel_unshuffle(x, scale=4)
        else:
            feat = x
        feat = self.conv_first(feat)
        body_feat = self.conv_body(self.body(feat))
        feat = feat + body_feat
        feat = self.lrelu(self.conv_up1(F.interpolate(feat, scale_factor=2, mode="nearest")))
        feat = self.lrelu(self.conv_up2(F.interpolate(feat, scale_factor=2, mode="nearest")))
        return self.conv_last(self.lrelu(self.conv_hr(feat)))


# ---- SRVGGNetCompact (realesr-animevideov3) -----------------------------------------------------------------

class SRVGGNetCompact(nn.Module):
    def __init__(self, num_in_ch=3, num_out_ch=3, num_feat=64, num_conv=16, upscale=4, act_type="prelu"):
        super().__init__()
        self.upscale = upscale
        self.body = nn.ModuleList()
        self.body.append(nn.Conv2d(num_in_ch, num_feat, 3, 1, 1))
        self.body.append(nn.PReLU(num_parameters=num_feat))
        for _ in range(num_conv):
            self.body.append(nn.Conv2d(num_feat, num_feat, 3, 1, 1))
            self.body.append(nn.PReLU(num_parameters=num_feat))
        self.body.append(nn.Conv2d(num_feat, num_out_ch * upscale * upscale, 3, 1, 1))
        self.upsampler = nn.PixelShuffle(upscale)

    def forward(self, x):
        out = x
        for layer in self.body:
            out = layer(out)
        out = self.upsampler(out)
        base = F.interpolate(x, scale_factor=self.upscale, mode="nearest")
        return out + base


# ---- Models -----------------------------------------------------------------------------------------------

MODELS = {
    # name: (builder, scale, filename)
    "animevideo": (lambda: SRVGGNetCompact(num_feat=64, num_conv=16, upscale=4), 4, "realesr-animevideov3.pth"),
    "x4plus": (lambda: RRDBNet(scale=4, num_block=23), 4, "RealESRGAN_x4plus.pth"),
    "anime6b": (lambda: RRDBNet(scale=4, num_block=6), 4, "RealESRGAN_x4plus_anime_6B.pth"),
    "x2plus": (lambda: RRDBNet(scale=2, num_block=23), 2, "RealESRGAN_x2plus.pth"),
}


@functools.lru_cache(maxsize=None)
def load_model(name: str, models_dir: str) -> Tuple[nn.Module, int]:
    builder, scale, filename = MODELS[name]
    model = builder()
    state = torch.load(os.path.join(models_dir, filename), map_location="cpu", weights_only=True)
    if "params_ema" in state:
        state = state["params_ema"]
    elif "params" in state:
        state = state["params"]
    model.load_state_dict(state, strict=True)
    model.eval()
    return model, scale


@torch.no_grad()
def upscale_array(rgb: np.ndarray, model: nn.Module, scale: int, tile: int = 128, pad: int = 8) -> np.ndarray:
    """rgb uint8 HxWx3 -> uint8 (H*scale)x(W*scale)x3, tiled with overlap so any size fits in memory."""
    h, w, _ = rgb.shape
    x = torch.from_numpy(rgb.astype(np.float32) / 255.0).permute(2, 0, 1).unsqueeze(0)
    out = torch.zeros((1, 3, h * scale, w * scale))
    for ty in range(0, h, tile):
        for tx in range(0, w, tile):
            y0, y1 = max(0, ty - pad), min(h, ty + tile + pad)
            x0, x1 = max(0, tx - pad), min(w, tx + tile + pad)
            patch = x[:, :, y0:y1, x0:x1]
            # Reflect-pad to a multiple of 4 so pixel-unshuffle variants work on any size.
            ph, pw = (-patch.shape[2]) % 4, (-patch.shape[3]) % 4
            if ph or pw:
                patch = F.pad(patch, (0, pw, 0, ph), mode="reflect")
            result = model(patch)[:, :, :(y1 - y0) * scale, :(x1 - x0) * scale]
            cy0, cx0 = (ty - y0) * scale, (tx - x0) * scale
            cy1, cx1 = cy0 + (min(h, ty + tile) - ty) * scale, cx0 + (min(w, tx + tile) - tx) * scale
            out[:, :, ty * scale:min(h, ty + tile) * scale, tx * scale:min(w, tx + tile) * scale] = result[:, :, cy0:cy1, cx0:cx1]
    return (out.squeeze(0).permute(1, 2, 0).clamp(0, 1).numpy() * 255.0 + 0.5).astype(np.uint8)


def upscale_image(image: Image.Image, model_name: str, models_dir: str, factor: int) -> Image.Image:
    """Upscales an RGBA image by ``factor`` (2 or 4) with the named model; alpha goes through the same net."""
    model, scale = load_model(model_name, models_dir)
    rgba = np.array(image.convert("RGBA"))
    rgb, alpha = rgba[:, :, :3], rgba[:, :, 3]
    # Transparent pixels carry garbage colour in many game textures; bleed the visible colour outward first
    # so the network does not sharpen the junk into visible fringes at the alpha edge.
    rgb = bleed_colour(rgb, alpha)
    up_rgb = upscale_array(rgb, model, scale)
    if alpha.min() == 255:
        up_alpha = np.full(up_rgb.shape[:2], 255, np.uint8)
    else:
        up_alpha = upscale_array(np.repeat(alpha[:, :, None], 3, axis=2), model, scale)[:, :, 0]
    result = Image.fromarray(np.dstack([up_rgb, up_alpha]), "RGBA")
    if factor != scale:
        result = result.resize((image.width * factor, image.height * factor), Image.LANCZOS)
    return result


def bleed_colour(rgb: np.ndarray, alpha: np.ndarray, iterations: int = 6) -> np.ndarray:
    """Fills fully transparent pixels with the nearest visible colours (a few dilation passes)."""
    if alpha.min() > 0:
        return rgb
    result = rgb.astype(np.float32)
    known = alpha > 0
    if not known.any():
        return rgb
    for _ in range(iterations):
        if known.all():
            break
        acc = np.zeros_like(result)
        cnt = np.zeros(known.shape, np.float32)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dy == 0 and dx == 0:
                    continue
                shifted = np.roll(np.roll(result, dy, axis=0), dx, axis=1)
                shifted_known = np.roll(np.roll(known, dy, axis=0), dx, axis=1)
                acc += shifted * shifted_known[:, :, None]
                cnt += shifted_known
        fill = ~known & (cnt > 0)
        result[fill] = acc[fill] / cnt[fill][:, None]
        known = known | fill
    return result.astype(np.uint8)
