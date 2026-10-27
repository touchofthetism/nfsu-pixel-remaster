## Install on the Pixel

1. Download the `NFSU-Remastered-…-arm64.apk` asset below on the phone and open it (allow installs from Chrome once).
2. Open **NFS Underground Remastered** → **Allow access to Internal storage/nfsu1** → confirm "All files access".
3. **Import game files (ZIP)** → pick `nfsu-gamedata.zip` (from the private *gamedata* release, downloaded on the phone, or straight from Drive). The launcher unpacks it into place and verifies `Speed.exe`.
4. **PLAY.** After a session, **Share diagnostics** sends the log ZIP wherever you like.

## What's new

- English launcher and in-game dialogs (Spanish kept as a locale); the game's text follows the phone's language.
- Pixel-class defaults: native resolution on phones with 8 GB+ RAM, 120 FPS cap, widescreen HUD on.
- **Texture filtering** option: 8×/16× anisotropic filtering across the whole game (default 16×).
- **Anti-aliasing** option: 2×/4× MSAA on the game's back buffer, validated against the GPU, off until you turn it on.
- **Import game files**: unpack the game ZIP directly from Drive/Downloads into Internal storage/nfsu1.
- **Share diagnostics**: one-tap log ZIP (log, device, options, exit records, graphics cache) through the share sheet.
- Signed with a stable key and a growing versionCode: every build installs over the previous one.
- Built from source on GitHub Actions: Speed.exe 1.4 → C → ARM64, DXVK native 1.9.2b on Vulkan.
