# Private companion repository — complete contents, mirrored here for review

The APK is built by a second, **private** repository (`touchofthetism/Nfsu-pixel-remaster-private-`) because it
holds binary assets that must not sit in a public repository: the owner's game folder and `Speed.exe` + sound
banks (as release assets), the APK signing key, and the built APKs. Everything that repository *does* is in
this folder, byte for byte, so the whole system can be read and audited from the public code alone:

- `.github/workflows/gamedata.yml` — once: fetch the owner's game copy, verify `Speed.exe` 1.4 (size, PE header,
  SHA-256), package `lift-inputs.zip` (recompiler inputs) and `nfsu-gamedata.zip` (for the launcher's import),
  publish them as private release assets.
- `.github/workflows/build.yml` — on a change to `BUILD`: check out this public repository at the named commit,
  download `lift-inputs.zip`, run `ci/setup_siblings.sh` + `ci/lift.sh`, assemble the signed ARM64 APK, publish
  it as a private release with unstripped symbols. The signing key is generated on the first run and kept as a
  private release asset (repository secrets take precedence when set).
- `BUILD` — the build manifest (`ref=`, `lto=`).
- `ci/gamedata.source` — the pointer to the owner's copy (the Drive id itself is only in the private repository).
- `ci/package_gamedata.py` lives at the repository root (`ci/package_gamedata.py`); the companion uses the same file.

Nothing else exists there.
