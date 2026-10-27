#!/usr/bin/env python3
"""Turn an extracted Need for Speed Underground (PC) install into the two inputs this project uses.

  package_gamedata.py <game_root> <out_dir> [--expect-sha256 HEX]

Writes to <out_dir>:
  lift-inputs.zip     Speed.exe + Sound/**/*.abk — everything the recompiler (scripts/run_lift.py,
                      scripts/abk_module.py) reads. ~30 MB. Consumed by the build workflow.
  nfsu-gamedata.zip   The game folder the launcher imports into Internal storage/nfsu1 (game root at
                      the ZIP root: Speed.exe, Cars, FrontEnd, Global, Languages, Movies, Sound,
                      SNDSTREAMS, SpeechStreams, Tracks, fx). ~1 GB.
  SHA256SUMS          Digests of both files.

Verifies the executable first: size 3178496 bytes, PE SizeOfImage 0x394368 and entry point 0x270CB5
(the 1.4 build; see android/app/src/main/cpp/game_data.h and scripts/abk_module.py), and the SHA-256
when one is given. PC-only extras (installer Support/, DRM driver, ASI plug-ins) are left out.
"""
import argparse
import hashlib
import os
import struct
import sys
import zipfile

PHONE_ENTRIES = ["Speed.exe", "Cars", "FrontEnd", "Global", "Languages", "Movies", "Sound",
                 "SNDSTREAMS", "SpeechStreams", "Tracks", "fx"]
SPEED_EXE_SIZE = 3178496
SPEED_EXE_IMAGE_SIZE = 0x394368
SPEED_EXE_ENTRY = 0x270CB5


def find_case_insensitive(root, name):
    for entry in os.listdir(root):
        if entry.lower() == name.lower():
            return os.path.join(root, entry)
    return None


def pe_image_size_and_entry(data):
    """SizeOfImage and AddressOfEntryPoint from a PE32 header (no third-party parser needed)."""
    if data[:2] != b"MZ":
        raise SystemExit("Speed.exe is not a PE executable (no MZ header)")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise SystemExit("Speed.exe is not a PE executable (no PE signature)")
    optional = pe_offset + 24
    entry = struct.unpack_from("<I", data, optional + 16)[0]
    image_size = struct.unpack_from("<I", data, optional + 56)[0]
    return image_size, entry


def verify_executable(path, expected_sha256):
    data = open(path, "rb").read()
    digest = hashlib.sha256(data).hexdigest()
    image_size, entry = pe_image_size_and_entry(data)
    problems = []
    if len(data) != SPEED_EXE_SIZE:
        problems.append(f"size {len(data)} (expected {SPEED_EXE_SIZE})")
    if image_size != SPEED_EXE_IMAGE_SIZE:
        problems.append(f"SizeOfImage {image_size:#x} (expected {SPEED_EXE_IMAGE_SIZE:#x})")
    if entry != SPEED_EXE_ENTRY:
        problems.append(f"entry point {entry:#x} (expected {SPEED_EXE_ENTRY:#x})")
    if expected_sha256 and digest.lower() != expected_sha256.lower():
        problems.append(f"sha256 {digest} (expected {expected_sha256})")
    if problems:
        raise SystemExit("Speed.exe is not the 1.4 build this port targets: " + "; ".join(problems))
    print(f"[*] Speed.exe OK: {len(data)} bytes, sha256 {digest}")
    return digest


def add_tree(zip_file, path, arcname):
    if os.path.isfile(path):
        zip_file.write(path, arcname)
        return 1
    count = 0
    for directory, _, files in os.walk(path):
        for name in sorted(files):
            full = os.path.join(directory, name)
            zip_file.write(full, os.path.join(arcname, os.path.relpath(full, path)))
            count += 1
    return count


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("game_root")
    parser.add_argument("out_dir")
    parser.add_argument("--expect-sha256", default="")
    args = parser.parse_args()

    exe = find_case_insensitive(args.game_root, "Speed.exe")
    if not exe:
        raise SystemExit(f"No Speed.exe in {args.game_root}")
    verify_executable(exe, args.expect_sha256)
    sound = find_case_insensitive(args.game_root, "Sound")
    if not sound or not os.path.isdir(sound):
        raise SystemExit("No Sound folder (the .abk banks are required by the recompiler)")

    os.makedirs(args.out_dir, exist_ok=True)
    lift = os.path.join(args.out_dir, "lift-inputs.zip")
    with zipfile.ZipFile(lift, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zip_file:
        zip_file.write(exe, "Speed.exe")
        banks = 0
        for directory, _, files in os.walk(sound):
            for name in sorted(files):
                if name.lower().endswith(".abk"):
                    full = os.path.join(directory, name)
                    zip_file.write(full, os.path.join("Sound", os.path.relpath(full, sound)))
                    banks += 1
    if banks == 0:
        raise SystemExit("Sound folder holds no .abk banks")
    print(f"[*] lift-inputs.zip: Speed.exe + {banks} sound banks, {os.path.getsize(lift) / 1048576:.1f} MB")

    phone = os.path.join(args.out_dir, "nfsu-gamedata.zip")
    missing = []
    with zipfile.ZipFile(phone, "w", zipfile.ZIP_DEFLATED, compresslevel=5) as zip_file:
        files = 0
        for entry in PHONE_ENTRIES:
            path = find_case_insensitive(args.game_root, entry)
            if not path:
                missing.append(entry)
                continue
            files += add_tree(zip_file, path, entry)
    required = {"Speed.exe", "Cars", "FrontEnd", "Global", "Languages", "Movies", "Sound", "Tracks"}
    if required & set(missing):
        raise SystemExit("Game folder is incomplete, missing: " + ", ".join(sorted(required & set(missing))))
    if missing:
        print("[!] optional folders not present: " + ", ".join(missing))
    print(f"[*] nfsu-gamedata.zip: {files} files, {os.path.getsize(phone) / 1048576:.1f} MB")

    with open(os.path.join(args.out_dir, "SHA256SUMS"), "w") as sums:
        for name in ("lift-inputs.zip", "nfsu-gamedata.zip"):
            sums.write(f"{sha256_file(os.path.join(args.out_dir, name))}  {name}\n")
    print(open(os.path.join(args.out_dir, "SHA256SUMS")).read(), end="")


if __name__ == "__main__":
    main()
