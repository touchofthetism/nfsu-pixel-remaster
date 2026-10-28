"""Builds an HD texture pack for the Android port from the game's own files.

For each listed bundle the texture packs are parsed (tpk.py), every eligible texture is upscaled with
Real-ESRGAN (upscale.py), re-encoded as DXT3 (alpha, the game's own alpha format; the stock data never uses DXT5) or DXT1 (opaque) with a
mip chain, and the
bundle is written back uncompressed (the engine accepts raw chunk streams under the .lzc name, as the
game's own modded copies and the Free Roam mod show). The result is a ZIP the launcher installs as a mod
pack: files at their game-folder paths plus ``nfsu-modpack.json`` describing them.

Profiles: hud (gauges), ui (the whole interface), cars (Cars/*/TEXTURES.BIN), all (ui + cars), tracks (the
city: every section of the track streams, the race tables that place them, and each race's sky).

Usage:
  hdpack.py --game <game folder> --out <pack.zip> --models <dir with Real-ESRGAN .pth> [--profile hud|ui|cars|all|tracks]
  hdpack.py --game <game folder> --models <dir> --cache <dir> --shard K/N [--profile ...]   (upscale one share)
  hdpack.py --game <game folder> --out <pack.zip> --cache <dir> [--profile ...]             (pack from the shares)

The sharper models are slow on a CPU (Real-ESRGAN x4plus: a few thousand input pixels a second on two
cores), so the work can be split: every texture to upscale becomes a job keyed by its source bytes, the
factor, the models, the edge handling and UPSCALE_VERSION; ``--shard K/N`` runs the K-th of N cost-balanced
shares and writes each result as a ready DDS (mip chain encoded) to ``--cache``; the pack is then built from
the cache with no upscaling at all. A key that is already in the cache is never upscaled again.

Track streams (Tracks/STREAM*.BUN) are rebuilt section by section and laid out again by tracks.py, which
moves every race's section table (Tracks/TRACKB*.lzc) to the new layout: the game sizes its streaming memory
from those tables, so bigger textures need no change to the game. World textures are upscaled with their
own continuation past the edges the game wraps them across (the TilableUV flag in each texture record) and
come out tiling without seams (tiling.py).

Memory discipline (32-bit guest, phone): the upscale factor depends on the texture's size, and the
output edge is capped, so a pack stays within a few tens of MB of texture memory per bundle.
"""
from __future__ import annotations

import argparse
import dataclasses
import functools
import hashlib
import io
import json
import os
import pathlib
import struct
import sys
import time
import zipfile
from typing import Callable, Optional

from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from nfstex import tiling, tpk, tracks  # noqa: E402
from nfstex.tiling import WRAP_BAND, WRAP_MARGIN  # noqa: E402

# Bundles by profile: (game-relative path, output name, default upscale policy name).
PROFILES = {
    "hud": [("Global/HUDTEX.BIN", "Global/HUDTEX.BIN", "hud")],
    # INGAMEB ships under both names (the engine prefers the .LZC when present): write both.
    "ui": [("Global/HUDTEX.BIN", "Global/HUDTEX.BIN", "hud"),
           ("Global/INGAMEB.BUN", "Global/InGameB.lzc", "ui"),
           ("Global/INGAMEB.BUN", "Global/INGAMEB.BUN", "ui"),
           ("FrontEnd/FrontB.lzc", "FrontEnd/FrontB.lzc", "ui"),
           # 159 track previews, 512x256 each, shown in a panel about 1.2x their size on the phone: 2x.
           ("FrontEnd/TSELECT.BIN", "FrontEnd/TSELECT.BIN", "preview"),
           ("FrontEnd/Road_Segment.bin", "FrontEnd/Road_Segment.bin", "photo")],
    # The 153 magazine covers (512x512) would grow to 164 MB at 2x for screens seen once per win: left stock.
}
# Car detail textures (lights, badges, interiors): every Cars/<car>/TEXTURES.BIN found in the game folder.
PROFILES["cars"] = [("Cars/*/TEXTURES.BIN", "Cars/*/TEXTURES.BIN", "car")]
PROFILES["all"] = PROFILES["ui"] + PROFILES["cars"]
# The city: every section of every track stream (roads, buildings, scenery, the start-line crowd), with the
# race tables moved to match, and the sky in each race's Tracks/TEX<race>TRACK.BIN.
PROFILES["tracks"] = [(tracks.STREAMS, tracks.STREAMS, "track"),
                      ("Tracks/TEX*TRACK.BIN", "Tracks/TEX*TRACK.BIN", "sky")]
PACK_NAMES = {"hud": "HD interface pack", "ui": "HD interface pack", "cars": "HD cars pack",
              "all": "HD interface and cars pack", "tracks": "HD tracks pack"}


def sky_texture(name: str) -> bool:
    """The sky dome's panorama, cap and moon in Tracks/TEX<race>TRACK.BIN; not the reflection maps
    (SKY_*_ENVMAP*, which the car shader samples blurred anyway) nor the HUD's track map (TRACK<race>_MAP)."""
    return name.startswith("SKY_") and "ENVMAP" not in name


@dataclasses.dataclass(frozen=True)
class Policy:
    """How a bundle's textures grow. The factor goes by the longer edge (``f128`` up to 128 pixels, ``f256`` up
    to 256, ``fbig`` above) and halves until the result fits ``max_edge``; a long thin texture (one edge at
    least four times the other) may reach ``strip_edge`` instead, within ``max_area`` pixels."""
    max_edge: int
    f128: int
    f256: int
    fbig: int
    strip_edge: int = 0
    max_area: int = 1 << 20
    formats: tuple = ()                 # stored formats upscaled (empty: all supported); others kept as stored
    names: Optional[Callable[[str], bool]] = None   # which textures by name (None: all)
    keep_uncompressed: bool = False     # A8R8G8B8 sources stay A8R8G8B8 (a sky's gradients band in DXT)
    wrap: bool = False                  # pad every edge, wrapping where the game wraps the texture (tiling.py)
    fix_colour: bool = False            # take the nets' colour drift back out, region by region (tiling.colour_fix)


POLICIES = {
    "hud": Policy(1024, 4, 4, 2),
    "ui": Policy(1024, 4, 2, 2),
    # World photographs (Road_Segment): 512-px ones stay as they are.
    "photo": Policy(512, 4, 2, 1),
    # Track previews: 512x256 -> 1024x512 (the whole set is 54 MB of DXT1, resident only in the front end).
    "preview": Policy(1024, 4, 2, 2),
    # Cars: small detail maps 4x, 256-px ones 2x, the few 512-px ones as they are (several cars stay loaded).
    # Only DXT-stored car textures: the game keeps lights, badges and rims uncompressed and works on some of
    # them on the CPU; a pack that re-encoded them as DXT (cars v4) overflowed a buffer at the profile menu
    # (ICALL unresolved into the heap from the sound service thread).
    "car": Policy(512, 4, 2, 1, formats=(tpk.COMP_DXT1, tpk.COMP_DXT3)),
    # The city: up to 128 px 4x, 256 and 512 px 2x, 1024-px textures kept, except the long road strips
    # (128x1024 -> 256x2048: the road surface is the most-seen texture in a race). DXT only: the six
    # paletted world textures (a motel sign, a billboard truck) stay as stored. About 113 MB of streaming
    # memory for a city race (21 stock), all of it from the race tables.
    # Colour fixed: the city keeps its look (x4plus/x2plus darken busy photographs by a few levels).
    "track": Policy(1024, 4, 2, 2, strip_edge=2048, formats=(tpk.COMP_DXT1, tpk.COMP_DXT3, tpk.COMP_DXT5), wrap=True,
                    fix_colour=True),
    # The sky: 512x128 panorama -> 1024x256, 256-px cap -> 512, moon 4x; uncompressed as stored.
    "sky": Policy(1024, 4, 2, 2, names=sky_texture, keep_uncompressed=True, wrap=True, fix_colour=True),
}

# Models. Colour goes through Real-ESRGAN's general models, x4plus for a 4x target and x2plus for 2x: on the
# game's photographs (track previews, car parts, portraits) they recover detail that the compact anime-tuned
# net of packs v1-v9 smoothed into paint, and flat art comes out as clean; x2plus works on a half-resolution
# unshuffle, a quarter of x4plus's cost. Alpha (masks and cut-outs) goes through the compact animevideo net:
# edges as clean at a thirtieth of the cost. --fast uses animevideo for everything, as packs v1-v9 did.
COLOUR_MODELS = {4: "x4plus", 2: "x2plus"}
ALPHA_MODEL = "animevideo"
FAST_MODEL = "animevideo"
# Part of every job key: bump it whenever the same source would come out differently (models, the colour
# bleed, the tiling, the DXT encoder, the mip rules), so no stale result is ever reused.
UPSCALE_VERSION = "3"
# Relative cost per input pixel, measured on two cores, for balancing shards.
MODEL_COST = {"x4plus": 7.6, "x2plus": 1.0, "general": 0.5, "animevideo": 0.25, "anime6b": 2.5}
SUPPORTED = (tpk.COMP_DXT1, tpk.COMP_DXT3, tpk.COMP_DXT5, tpk.COMP_P8, tpk.COMP_A8R8G8B8)


def should_skip(texture: tpk.Texture, policy: Policy) -> str:
    """Why a texture is left as stored under the policy, or "" when it is upscaled."""
    w, h = texture.width, texture.height
    if w < 8 or h < 8:
        return "tiny"
    if w > 1024 or h > 1024:
        return "already large"
    if texture.compression not in SUPPORTED:
        return "unsupported format"
    if policy.formats and texture.compression not in policy.formats:
        return "stored format left as is under this policy"
    if policy.names and not policy.names(texture.name):
        return "not one of this policy's textures"
    return ""


def factor_for(policy: Policy, width: int, height: int) -> int:
    edge = max(width, height)
    factor = policy.f128 if edge <= 128 else policy.f256 if edge <= 256 else policy.fbig
    limit = policy.strip_edge if policy.strip_edge and edge >= 4 * min(width, height) else policy.max_edge
    while factor > 1 and (edge * factor > limit or width * height * factor * factor > policy.max_area):
        factor //= 2
    return factor


def wrap_axes(texture: tpk.Texture, policy: Policy) -> Optional[str]:
    """None: the edges are left to the nets (interface and car packs). Otherwise the axes the game wraps the
    texture along, from its TilableUV flag: "x" across the left and right edges, "y" across the top and bottom."""
    if not policy.wrap:
        return None
    flag = texture.tilable_uv
    return ("x" if flag & 1 else "") + ("y" if flag & 2 else "")


def downsample(image: Image.Image, size, wrap: Optional[str]) -> Image.Image:
    """``image`` resized to ``size`` (LANCZOS); along the axes in ``wrap`` the filter reads past each edge into
    the texture's continuation, so the mip levels of a tiling texture tile too (tiling.resize)."""
    return tiling.resize(image, size, wrap)


def mip_chain_dds(image: Image.Image, compression: int, wrap: Optional[str] = None) -> bytes:
    """A DDS with the mip chain in the game's format, encoded level by level: DXT1/DXT3 (nfstex.dxt) or
    uncompressed A8R8G8B8 (B,G,R,A bytes) for textures the game stores uncompressed, whose 8-bit alpha
    gradients (light glows) would band at DXT3's 4 bits. ``wrap``: see downsample."""
    import numpy as np
    from nfstex import dxt
    levels = []
    w, h = image.size
    source = image.convert("RGBA")
    current = source
    # The game's own chains stop when the smaller edge reaches 8 pixels (128x128 -> 5 levels, 256x64 -> 4):
    # keep that convention so the engine's size arithmetic for the chain matches the data.
    while True:
        pixels = np.array(current)
        if compression == tpk.COMP_A8R8G8B8:
            levels.append(np.ascontiguousarray(pixels[..., [2, 1, 0, 3]]).tobytes())
        else:
            levels.append(dxt.encode_dxt1(pixels) if compression == tpk.COMP_DXT1 else dxt.encode_dxt3(pixels))
        if min(w, h) <= 8:
            break
        w, h = max(1, w // 2), max(1, h // 2)
        current = downsample(source, (w, h), wrap)
    mips = len(levels)
    if compression == tpk.COMP_A8R8G8B8:
        flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x8
        pitch = image.width * 4
        pf = struct.pack("<IIIIIIII", 32, 0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    else:
        flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000
        pitch = max(1, (image.width + 3) // 4) * (8 if compression == tpk.COMP_DXT1 else 16)
        pf = struct.pack("<II4sIIIII", 32, 0x4, {tpk.COMP_DXT1: b"DXT1", tpk.COMP_DXT3: b"DXT3"}[compression], 0, 0, 0, 0, 0)
    header = struct.pack("<IIIIII", 0x7C, flags, image.height, image.width, pitch, 0) + struct.pack("<I", mips) + b"\0" * 44 + pf + struct.pack("<IIIII", 0x401008, 0, 0, 0, 0)
    return b"DDS " + header + b"".join(levels)


def has_alpha(image: Image.Image) -> bool:
    extrema = image.getchannel("A").getextrema()
    return extrema[0] < 250


def soft_alpha(image: Image.Image) -> bool:
    """More distinct alpha values than a 4-bit channel can hold (a gradient rather than a mask)."""
    histogram = image.getchannel("A").histogram()
    return sum(1 for count in histogram if count) > 17


def models_for(factor: int, fast: bool) -> tuple[str, str]:
    """(colour model, alpha model) for an upscale factor."""
    return (FAST_MODEL, FAST_MODEL) if fast else (COLOUR_MODELS[factor], ALPHA_MODEL)


@dataclasses.dataclass
class Job:
    """One texture to upscale. The key covers everything the result depends on."""
    key: str
    relative: str
    texture: tpk.Texture
    factor: int
    colour: str
    alpha: str
    wrap: Optional[str] = None          # None: unpadded edges; else the wrapping axes (see wrap_axes)
    keep_uncompressed: bool = False
    fix_colour: bool = False

    @property
    def cost(self) -> float:
        width, height = self.texture.width, self.texture.height
        if self.wrap is not None:
            width, height = width + 2 * WRAP_MARGIN, height + 2 * WRAP_MARGIN
        pixels = width * height
        alpha = 0.0 if self.texture.compression == tpk.COMP_DXT1 else MODEL_COST.get(self.alpha, 1.0)
        return pixels * (MODEL_COST.get(self.colour, 1.0) + alpha)


def job_key(texture: tpk.Texture, factor: int, colour: str, alpha: str, wrap: Optional[str] = None,
            keep_uncompressed: bool = False, fix_colour: bool = False) -> str:
    text = (f"nfstex-hd {UPSCALE_VERSION} {texture.width}x{texture.height} {texture.compression} "
            f"x{factor} {colour} {alpha}")
    if wrap is not None:
        text += f" edges '{wrap}' {WRAP_MARGIN}/{WRAP_BAND}"
    if keep_uncompressed:
        text += " keep-uncompressed"
    if fix_colour:
        text += " colour-fix"
    digest = hashlib.sha1(text.encode())
    digest.update(texture.pixels)
    digest.update(texture.palette)
    return digest.hexdigest()


def load_bundle(game: pathlib.Path, relative: str):
    """(decompressed data, [(offset, chunk)], [TexturePack]) of a game file, found case-insensitively."""
    path = game / relative
    if not path.is_file():
        for candidate in path.parent.iterdir() if path.parent.is_dir() else []:
            if candidate.name.lower() == path.name.lower():
                path = candidate
                break
    data = path.read_bytes()
    if tpk.is_jdlz(data):
        data = tpk.jdlz_decompress(data)
    chunks = tpk.find_texture_packs(data)
    return data, chunks, [tpk.TexturePack.parse(chunk, data, offset) for offset, chunk in chunks]


def profile_bundles(game: pathlib.Path, profile: str, only: str | None = None):
    """[(input path, policy, [output paths])] for a profile, globs expanded, each input listed once. ``only``:
    comma-separated profile entries or expanded paths (any case) to keep."""
    wanted = {name.strip().lower() for name in only.split(",")} if only else None
    found = {}
    for relative, output, policy in PROFILES[profile]:
        if "*" in relative:
            paths = [(path.relative_to(game).as_posix(),) * 2 for path in sorted(game.glob(relative))]
        else:
            paths = [(relative, output)]
        for rel, out in paths:
            if wanted and rel.lower() not in wanted and relative.lower() not in wanted:
                continue
            found.setdefault(rel, (policy, []))[1].append(out)
    return [(rel, policy, outputs) for rel, (policy, outputs) in found.items()]


@functools.lru_cache(maxsize=2)
def _stream_layout(game: str):
    """{stream path: {offset: section entry}} from every race table of a game folder (tracks.layouts)."""
    folder = pathlib.Path(game)
    return tracks.layouts(folder, tracks.find_races(folder))


def bundle_units(game: pathlib.Path, relative: str):
    """(label, data, chunks, packs) for each unit the bundle is rebuilt in: an ordinary bundle is one unit; a
    track stream one unit per section that holds texture packs (tracks.build lays the sections out again)."""
    if not tracks.is_stream(relative):
        yield (relative,) + load_bundle(game, relative)
        return
    sections = _stream_layout(str(game)).get(relative, {})
    if not sections:
        return
    data = (game / relative).read_bytes()
    for offset in sorted(sections):
        entry = sections[offset]
        section = data[offset:offset + entry.size]
        chunks = tpk.find_texture_packs(section)
        if chunks:
            yield f"{relative} {entry.name}", section, chunks, [tpk.TexturePack.parse(c, section, o) for o, c in chunks]


def bundle_jobs(relative: str, packs, policy_name: str, limit: int = 0, fast: bool = False):
    """The jobs of one unit, in texture order (limit > 0: the first `limit` eligible textures only)."""
    policy = POLICIES[policy_name]
    jobs = []
    for pack in packs:
        for texture in pack.textures:
            if should_skip(texture, policy):
                continue
            factor = factor_for(policy, texture.width, texture.height)
            if factor < 2 or (limit and len(jobs) >= limit):
                continue
            colour, alpha = models_for(factor, fast)
            wrap = wrap_axes(texture, policy)
            key = job_key(texture, factor, colour, alpha, wrap, policy.keep_uncompressed, policy.fix_colour)
            jobs.append(Job(key, relative, texture, factor, colour, alpha, wrap, policy.keep_uncompressed, policy.fix_colour))
    return jobs


def plan(game: pathlib.Path, profile: str, only: str | None = None, limit: int = 0, fast: bool = False):
    """Every distinct job of a profile, in a fixed order (the same in every shard and in the pack step)."""
    jobs, seen = [], set()
    for relative, policy, _ in profile_bundles(game, profile, only):
        for label, _, _, packs in bundle_units(game, relative):
            for job in bundle_jobs(label, packs, policy, limit, fast):
                if job.key not in seen:
                    seen.add(job.key)
                    jobs.append(job)
    return jobs


def assign_shards(jobs, count: int):
    """Splits the jobs into `count` shares of near-equal cost: largest first, each to the lightest share
    (ties to the lower index). Deterministic for the same plan."""
    shares = [[] for _ in range(count)]
    loads = [0.0] * count
    for job in sorted(jobs, key=lambda j: (-j.cost, j.key)):
        lightest = min(range(count), key=lambda i: (loads[i], i))
        shares[lightest].append(job)
        loads[lightest] += job.cost
    return shares, loads


class Cache:
    """Finished textures as DDS files named by job key, in one directory."""

    def __init__(self, directory: str | None):
        self.directory = pathlib.Path(directory) if directory else None
        if self.directory:
            self.directory.mkdir(parents=True, exist_ok=True)

    def get(self, key: str) -> bytes | None:
        if not self.directory:
            return None
        path = self.directory / f"{key}.dds"
        return path.read_bytes() if path.is_file() else None

    def put(self, key: str, dds: bytes) -> None:
        if not self.directory:
            return
        temp = self.directory / f"{key}.dds.tmp"
        temp.write_bytes(dds)
        os.replace(temp, self.directory / f"{key}.dds")


def build_dds(job: Job, models_dir: str) -> bytes:
    """Upscales one texture and encodes it in the format family it was stored in. DXT sources stay DXT (DXT3
    when there is alpha); uncompressed sources stay A8R8G8B8 when their alpha is a gradient or the policy keeps
    them uncompressed, and go DXT when it is a hard mask or absent."""
    from nfstex import upscale  # torch import deferred: only needed when upscaling
    texture = job.texture
    image = Image.open(io.BytesIO(texture.to_dds())).convert("RGBA")
    up = upscale.upscale_image(image, job.colour, models_dir, job.factor, alpha_model=job.alpha, wrap=job.wrap,
                               fix_colour=job.fix_colour)
    if texture.compression == tpk.COMP_A8R8G8B8 and (job.keep_uncompressed or soft_alpha(image)):
        compression = tpk.COMP_A8R8G8B8
    else:
        compression = tpk.COMP_DXT3 if has_alpha(image) else tpk.COMP_DXT1
    return mip_chain_dds(up, compression, job.wrap)


def dds_summary(dds: bytes) -> str:
    """"1024x512 DXT1" from a DDS header."""
    height, width = struct.unpack_from("<II", dds, 12)
    fourcc = dds[84:88]
    return f"{width}x{height} {fourcc.decode('ascii') if fourcc in (b'DXT1', b'DXT3', b'DXT5') else 'A8R8G8B8'}"


def run_shard(jobs, index: int, count: int, cache: Cache, models_dir: str, log) -> int:
    """Upscales share `index` of `count` into the cache; returns how many were upscaled (cached ones skipped)."""
    shares, loads = assign_shards(jobs, count)
    mine = shares[index]
    log(f"shard {index + 1}/{count}: {len(mine)} of {len(jobs)} jobs, cost {loads[index] / 1e6:.1f} "
        f"(shares {min(loads) / 1e6:.1f}-{max(loads) / 1e6:.1f})")
    started, done = time.time(), 0
    for number, job in enumerate(mine, 1):
        if cache.get(job.key) is not None:
            continue
        dds = build_dds(job, models_dir)
        cache.put(job.key, dds)
        done += 1
        log(f"  {job.relative}: {job.texture.name} {job.texture.width}x{job.texture.height} -> {dds_summary(dds)} "
            f"{job.colour}/{job.alpha}{'' if job.wrap is None else ' edges ' + (job.wrap or 'mirrored')} "
            f"({number}/{len(mine)}, {time.time() - started:.0f}s)")
    return done


def rebuild_data(label: str, data: bytes, chunks, packs, policy_name: str, models_dir: str | None, log,
                 limit: int = 0, cache: Cache | None = None, fast: bool = False, verbose: bool = True) -> bytes | None:
    """``data`` (a bundle, or one track section) rebuilt with its textures upscaled, or None when no texture
    qualified (the stock bytes are then left alone).

    Each upscaled texture comes from the cache when it holds the job's key, else is upscaled here (and
    stored in the cache) when models are given; a missing result without models stops the build.
    limit > 0 upscales only the first `limit` eligible textures (for local checks of a policy). ``verbose``
    logs every texture, not just the summary line."""
    cache = cache or Cache(None)
    jobs = bundle_jobs(label, packs, policy_name, limit, fast)
    stats = {"textures": sum(len(p.textures) for p in packs), "upscaled": 0, "cached": 0,
             "bytes_in": sum(len(t.pixels) for p in packs for t in p.textures), "bytes_out": 0}
    started = time.time()
    for job in jobs:
        dds = cache.get(job.key)
        if dds is not None:
            stats["cached"] += 1
        elif models_dir:
            dds = build_dds(job, models_dir)
            cache.put(job.key, dds)
        else:
            raise SystemExit(f"{label}: {job.texture.name} ({job.key}) is not in the cache and no models were given")
        before = len(job.texture.pixels)
        job.texture.replace_with_dds(dds)
        stats["upscaled"] += 1
        stats["bytes_out"] += len(job.texture.pixels) - before
        if verbose:
            log(f"  {label}: {job.texture.name} -> {dds_summary(dds)} {job.colour}/{job.alpha} "
                f"({stats['upscaled']} done, {time.time() - started:.0f}s)")
    stats["bytes_out"] += stats["bytes_in"]
    kept = stats["textures"] - stats["upscaled"]
    if not stats["upscaled"]:
        log(f"{label}: nothing to upscale under policy {policy_name} ({kept} textures kept); not shipped")
        return None
    rebuilt = {offset: (lambda base, p=pack: p.serialize(base)) for (offset, _), pack in zip(chunks, packs)}
    out = tpk.replace_texture_packs(data, chunks, rebuilt)
    blobs = verify_rebuilt(label, data, out)
    log(f"{label}: {stats['upscaled']} upscaled ({stats['cached']} from the cache), {kept} kept, texture bytes "
        f"{stats['bytes_in'] >> 10} KB -> {stats['bytes_out'] >> 10} KB, {'section' if tracks.is_stream(label.split(' ')[0]) else 'file'} "
        f"{len(out) >> 10} KB, {blobs} JDLZ blobs pass the game's decoder")
    return out


def process_bundle(game: pathlib.Path, relative: str, policy_name: str, models_dir: str | None, log,
                   limit: int = 0, cache: Cache | None = None, fast: bool = False) -> bytes | None:
    """The rebuilt bundle, or None when no texture qualified (the stock file is then left alone); see
    rebuild_data."""
    data, chunks, packs = load_bundle(game, relative)
    return rebuild_data(relative, data, chunks, packs, policy_name, models_dir, log, limit, cache, fast)


def build_streams(game: pathlib.Path, relatives, policy_name: str, models_dir: str | None, log, limit: int = 0,
                  cache: Cache | None = None, fast: bool = False) -> dict:
    """{game path: bytes} for the track streams in ``relatives`` rebuilt section by section, and every race
    table they move (tracks.build); streams not listed keep their sections."""
    selected = {relative.lower() for relative in relatives}

    def rebuild_section(path: str, section: bytes, entry: tracks.Entry) -> bytes:
        if path.lower() not in selected:
            return section
        chunks = tpk.find_texture_packs(section)
        if not chunks:
            return section
        packs = [tpk.TexturePack.parse(chunk, section, offset) for offset, chunk in chunks]
        if any(pack.compressed_source for pack in packs):
            # Their blob tables hold absolute file offsets, which a section rebuilt on its own cannot know.
            raise SystemExit(f"{path} {entry.name}: a texture pack stored as compressed blobs in a stream section")
        out = rebuild_data(f"{path} {entry.name}", section, chunks, packs, policy_name, models_dir, log, limit,
                           cache, fast, verbose=False)
        return section if out is None else out

    return tracks.build(game, rebuild_section, log)


def verify_rebuilt(relative: str, source: bytes, out: bytes) -> int:
    """Every JDLZ blob of a rebuilt file must decode under Speed.exe's own rules (tpk.jdlz_game_decode),
    and the file must parse back into texture packs with the same textures by name. Returns the blob count.

    HD packs v1-v5 shipped blobs the game's decoder ran off the end of (one terminator token short), which
    overwrote the heap on the phone; this is the check that would have caught them."""
    try:
        blobs = tpk.verify_jdlz_blobs(out)
    except tpk.JdlzError as e:
        raise SystemExit(f"{relative}: {e}")
    before = [t.name for offset, chunk in tpk.find_texture_packs(source) for t in tpk.TexturePack.parse(chunk, source, offset).textures]
    after = [t.name for offset, chunk in tpk.find_texture_packs(out) for t in tpk.TexturePack.parse(chunk, out, offset).textures]
    if sorted(before) != sorted(after):
        raise SystemExit(f"{relative}: the rebuilt file lists different textures ({len(before)} -> {len(after)})")
    return blobs


def repack_file(relative: str, data: bytes, log) -> bytes:
    """A pack file from an earlier HD pack, written again by the current serializer (no upscaling): the way
    to re-ship a pack after a fix in the container or compression code."""
    if tpk.is_jdlz(data):
        data = tpk.jdlz_decompress(data)
    try:
        packs = tpk.find_texture_packs(data)
    except ValueError:
        packs = []                                  # not a chunk stream at all
    if not packs:
        log(f"{relative}: no texture packs, copied as is")
        return data
    rebuilt = {offset: (lambda base, p=tpk.TexturePack.parse(chunk, data, offset): p.serialize(base)) for offset, chunk in packs}
    out = tpk.replace_texture_packs(data, packs, rebuilt)
    blobs = verify_rebuilt(relative, data, out)
    log(f"{relative}: repacked, {len(data) >> 10} KB -> {len(out) >> 10} KB, {blobs} JDLZ blobs pass the game's decoder")
    return out


def repack(source_zip: str, out_zip: str, version: str, name: str | None, log) -> None:
    """Rebuilds every file of an existing HD pack ZIP with the current code and writes a new pack."""
    with zipfile.ZipFile(source_zip) as src:
        manifest = json.loads(src.read("nfsu-modpack.json"))
        files = {info.filename: repack_file(info.filename, src.read(info), log)
                 for info in src.infolist() if info.filename != "nfsu-modpack.json" and not info.is_dir()}
    previous = manifest.get("version", "?")
    manifest["version"] = version
    if name:
        manifest["name"] = name
    manifest["files"] = [{"path": p, "bytes": len(b)} for p, b in files.items()]
    manifest["notes"] = manifest.get("notes", "") + f" Repacked from version {previous}."
    with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_DEFLATED) as zf:
        for p, b in files.items():
            zf.writestr(p, b)
        zf.writestr("nfsu-modpack.json", json.dumps(manifest, indent=2))
    log(f"wrote {out_zip}: {len(files)} files, {sum(len(b) for b in files.values()) >> 20} MB uncompressed")


def build_pack(game: pathlib.Path, profile: str, models_dir: str | None, cache: Cache, log, only: str | None = None,
               limit: int = 0, fast: bool = False) -> dict:
    """{game path: bytes} of every file the profile changes."""
    bundles = profile_bundles(game, profile, only)
    files = {}
    streams = [(relative, policy) for relative, policy, _ in bundles if tracks.is_stream(relative)]
    if streams:
        policies = {policy for _, policy in streams}
        if len(policies) != 1:
            raise SystemExit(f"track streams under more than one policy: {sorted(policies)}")
        files.update(build_streams(game, [relative for relative, _ in streams], policies.pop(), models_dir, log,
                                   limit, cache, fast))
    for relative, policy, outputs in bundles:
        if tracks.is_stream(relative):
            continue
        built = process_bundle(game, relative, policy, models_dir, log, limit, cache, fast)
        if built is not None:
            for output in outputs:
                files[output] = built
    return files


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", help="game folder (holds Speed.exe)")
    parser.add_argument("--out", help="output ZIP")
    parser.add_argument("--models", help="directory with the Real-ESRGAN .pth files (not needed when the cache holds every job)")
    parser.add_argument("--profile", default="ui", choices=sorted(PROFILES))   # hud | ui | cars | all | tracks
    parser.add_argument("--name", default=None, help="pack name in the manifest (default: by profile, or the repacked pack's own)")
    parser.add_argument("--version", default="1")
    parser.add_argument("--only", help="comma-separated bundles to build, as profile entries or game paths (others skipped)")
    parser.add_argument("--limit", type=int, default=0, help="upscale at most N textures per bundle or track section (policy checks)")
    parser.add_argument("--cache", help="directory of finished textures (DDS by job key): read, and written when upscaling")
    parser.add_argument("--shard", metavar="K/N", help="upscale only the K-th (1-based) of N cost-balanced shares into --cache, then stop")
    parser.add_argument("--fast", action="store_true", help="the compact animevideo net for colour too (packs v1-v9), for quick checks")
    parser.add_argument("--repack", metavar="PACK.zip", help="rewrite an existing HD pack with the current code instead of upscaling (no game folder or models needed)")
    args = parser.parse_args()
    if args.repack:
        if not args.out:
            parser.error("--out is required")
        repack(args.repack, args.out, args.version, args.name, print)
        return
    if not args.game:
        parser.error("--game is required unless --repack is given")
    game = pathlib.Path(args.game)
    cache = Cache(args.cache)
    if args.shard:
        if not args.cache or not args.models:
            parser.error("--shard needs --cache and --models")
        index, count = (int(v) for v in args.shard.split("/"))
        if not 1 <= index <= count:
            parser.error("--shard K/N needs 1 <= K <= N")
        jobs = plan(game, args.profile, args.only, args.limit, args.fast)
        run_shard(jobs, index - 1, count, cache, args.models, print)
        return
    if not args.out:
        parser.error("--out is required")
    if not args.models and not args.cache:
        parser.error("--models or --cache is required")
    files = build_pack(game, args.profile, args.models, cache, print, args.only, args.limit, args.fast)
    if not files:
        sys.exit("no bundle changed: nothing to pack")
    colour = "Real-ESRGAN animevideo" if args.fast else "Real-ESRGAN x4plus (4x) and x2plus (2x), alpha through animevideo"
    notes = f"Upscaled with {colour} from the game's own textures; bundles rewritten uncompressed."
    if any(tracks.is_stream(path) for path in files):
        notes += (" Track streams rebuilt section by section, with every race's section table moved to match (the"
                  " game sizes its streaming memory from those tables); world textures tile as the game wraps them.")
    manifest = {
        "format": 1,
        "name": args.name or PACK_NAMES.get(args.profile, "HD texture pack"),
        "version": args.version,
        "game": "nfsu1",
        "notes": notes,
        "files": [{"path": p, "bytes": len(b)} for p, b in files.items()],
        # The uncompressed bundle replaces both names the game may look for.
        "remove": [],
    }
    # A rewritten Global/InGameB.lzc must also win over the stock INGAMEB.BUN: the engine prefers .LZC,
    # so nothing to remove; FrontB has no .BUN; HUDTEX has one name.
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as zf:
        for p, b in files.items():
            zf.writestr(p, b)
        zf.writestr("nfsu-modpack.json", json.dumps(manifest, indent=2))
    print(f"wrote {args.out}: {len(files)} files, {sum(len(b) for b in files.values()) >> 20} MB uncompressed")


if __name__ == "__main__":
    main()
