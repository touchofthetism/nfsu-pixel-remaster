"""Builds an HD texture pack for the Android port from the game's own files.

For each listed bundle the texture packs are parsed (tpk.py), every eligible texture is upscaled with
Real-ESRGAN (upscale.py), re-encoded as DXT3 (alpha, the game's own alpha format; the stock data never uses DXT5) or DXT1 (opaque) with a
mip chain, and the
bundle is written back uncompressed (the engine accepts raw chunk streams under the .lzc name, as the
game's own modded copies and the Free Roam mod show). The result is a ZIP the launcher installs as a mod
pack: files at their game-folder paths plus ``nfsu-modpack.json`` describing them.

Usage:
  hdpack.py --game <game folder> --out <pack.zip> --models <dir with Real-ESRGAN .pth> [--profile hud|ui|all]

Memory discipline (32-bit guest, phone): the upscale factor depends on the texture's size, and the
output edge is capped, so a pack stays within a few tens of MB of texture memory per bundle.
"""
from __future__ import annotations

import argparse
import io
import json
import os
import pathlib
import struct
import sys
import time
import zipfile

from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from nfstex import tpk  # noqa: E402

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

# Policy: (model, max output edge, factor for inputs <= 128, factor for inputs <= 256, factor above).
POLICIES = {
    "hud": ("animevideo", 1024, 4, 4, 2),
    "ui": ("animevideo", 1024, 4, 2, 2),
    # Photographs also go through the compact net: RRDBNet is 20x slower on a CPU and the gain on these
    # mostly-dark 2003 renders is small at 2x. 512-px ones stay as they are (world textures in Road_Segment).
    "photo": ("animevideo", 512, 4, 2, 1),
    # Track previews: 512x256 -> 1024x512 (the whole set is 54 MB of DXT1, resident only in the front end).
    "preview": ("animevideo", 1024, 4, 2, 2),
    # Cars: small detail maps 4x, 256-px ones 2x, the few 512-px ones as they are (several cars stay loaded).
    "car": ("animevideo", 512, 4, 2, 1),
}


def should_skip(texture: tpk.Texture, policy_name: str = "") -> str:
    w, h = texture.width, texture.height
    if w < 8 or h < 8:
        return "tiny"
    if w > 1024 or h > 1024:
        return "already large"
    if texture.compression not in (tpk.COMP_DXT1, tpk.COMP_DXT3, tpk.COMP_DXT5, tpk.COMP_P8, tpk.COMP_A8R8G8B8):
        return "unsupported format"
    # Car bundles: the game keeps lights, badges and rims uncompressed and works on some of them on the
    # CPU; a pack that re-encoded them as DXT (cars v4) overflowed a buffer at the profile menu
    # (ICALL unresolved into the heap from the sound service thread). Only the DXT-stored car textures are
    # touched, and they stay DXT.
    if policy_name == "car" and texture.compression not in (tpk.COMP_DXT1, tpk.COMP_DXT3):
        return "uncompressed or paletted car texture (left as stored)"
    return ""


def factor_for(policy, width: int, height: int) -> int:
    _, max_edge, f128, f256, fbig = policy
    edge = max(width, height)
    factor = f128 if edge <= 128 else f256 if edge <= 256 else fbig
    while edge * factor > max_edge and factor > 1:
        factor //= 2
    return factor


def mip_chain_dds(image: Image.Image, compression: int) -> bytes:
    """A DDS with the mip chain in the game's format, encoded level by level: DXT1/DXT3 (nfstex.dxt) or
    uncompressed A8R8G8B8 (B,G,R,A bytes) for textures the game stores uncompressed, whose 8-bit alpha
    gradients (light glows) would band at DXT3's 4 bits."""
    import numpy as np
    from nfstex import dxt
    levels = []
    w, h = image.size
    current = image.convert("RGBA")
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
        current = image.convert("RGBA").resize((w, h), Image.LANCZOS)
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


def process_bundle(game: pathlib.Path, relative: str, policy_name: str, models_dir: str, log,
                   limit: int = 0) -> bytes | None:
    """The rebuilt bundle, or None when no texture qualified (the stock file is then left alone).

    limit > 0 upscales only the first `limit` eligible textures (for local checks of a policy)."""
    from nfstex import upscale  # torch import deferred: only needed when building
    policy = POLICIES[policy_name]
    path = game / relative
    if not path.is_file():
        for candidate in path.parent.iterdir() if path.parent.is_dir() else []:
            if candidate.name.lower() == path.name.lower():
                path = candidate
                break
    data = path.read_bytes()
    if tpk.is_jdlz(data):
        data = tpk.jdlz_decompress(data)
    packs = tpk.find_texture_packs(data)
    rebuilt = {}
    stats = {"textures": 0, "upscaled": 0, "skipped": 0, "bytes_in": 0, "bytes_out": 0}
    started = time.time()
    for offset, chunk in packs:
        pack = tpk.TexturePack.parse(chunk, data, offset)
        for texture in pack.textures:
            stats["textures"] += 1
            stats["bytes_in"] += len(texture.pixels)
            reason = should_skip(texture, policy_name)
            if reason:
                stats["skipped"] += 1
                stats["bytes_out"] += len(texture.pixels)
                continue
            factor = factor_for(policy, texture.width, texture.height)
            if factor < 2 or (limit and stats["upscaled"] >= limit):
                stats["skipped"] += 1
                stats["bytes_out"] += len(texture.pixels)
                continue
            image = Image.open(io.BytesIO(texture.to_dds())).convert("RGBA")
            up = upscale.upscale_image(image, policy[0], models_dir, factor)
            # The output keeps the stored format family: DXT sources stay DXT (DXT3 when there is alpha),
            # uncompressed sources (interface bundles only; cars skip them above) stay A8R8G8B8 when their
            # alpha is a gradient and go DXT when it is a hard mask or absent.
            compression = tpk.COMP_A8R8G8B8 if texture.compression == tpk.COMP_A8R8G8B8 and soft_alpha(image) \
                else tpk.COMP_DXT3 if has_alpha(image) else tpk.COMP_DXT1
            texture.replace_with_dds(mip_chain_dds(up, compression))
            stats["upscaled"] += 1
            stats["bytes_out"] += len(texture.pixels)
            log(f"  {relative}: {texture.name} {image.width}x{image.height} -> {up.width}x{up.height} "
                f"{ {tpk.COMP_DXT3: 'DXT3', tpk.COMP_DXT1: 'DXT1', tpk.COMP_A8R8G8B8: 'A8R8G8B8'}[compression]} "
                f"({stats['upscaled']} done, {time.time() - started:.0f}s)")
        rebuilt[offset] = (lambda base, p=pack: p.serialize(base))
    if not stats["upscaled"]:
        log(f"{relative}: nothing to upscale under policy {policy_name} ({stats['skipped']} textures kept); not shipped")
        return None
    out = tpk.replace_texture_packs(data, packs, rebuilt)
    log(f"{relative}: {stats['upscaled']} upscaled, {stats['skipped']} kept, texture bytes {stats['bytes_in'] >> 10} KB -> {stats['bytes_out'] >> 10} KB, file {len(out) >> 10} KB")
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", required=True, help="game folder (holds Speed.exe)")
    parser.add_argument("--out", required=True, help="output ZIP")
    parser.add_argument("--models", required=True, help="directory with the Real-ESRGAN .pth files")
    parser.add_argument("--profile", default="ui", choices=sorted(PROFILES))   # hud | ui | cars | all
    parser.add_argument("--name", default="HD interface pack")
    parser.add_argument("--version", default="1")
    parser.add_argument("--only", help="comma-separated bundle names to build (others skipped)")
    parser.add_argument("--limit", type=int, default=0, help="upscale at most N textures per bundle (policy checks)")
    args = parser.parse_args()
    game = pathlib.Path(args.game)
    files = {}
    processed = {}     # one upscale per input, however many names it is written under
    for relative, output, policy in PROFILES[args.profile]:
        if args.only and relative not in args.only.split(","):
            continue
        if "*" in relative:
            for path in sorted(game.glob(relative)):
                rel = path.relative_to(game).as_posix()
                built = process_bundle(game, rel, policy, args.models, print, args.limit)
                if built is not None:
                    files[rel] = built
            continue
        if relative not in processed:
            processed[relative] = process_bundle(game, relative, policy, args.models, print, args.limit)
        if processed[relative] is not None:
            files[output] = processed[relative]
    if not files:
        sys.exit("no bundle changed: nothing to pack")
    manifest = {
        "format": 1,
        "name": args.name,
        "version": args.version,
        "game": "nfsu1",
        "notes": "Upscaled with Real-ESRGAN from the game's own textures; bundles rewritten uncompressed.",
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
