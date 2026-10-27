# Need for Speed Underground — Recompiled


### Android downloads / Descargas Android

[Underground 1 v0.2.5](https://github.com/codepdbh/nfsu1-recompiled/releases/tag/v0.2.5)

| APK | Architecture |
|---|---|
| [ARM64 / 64 bits](https://github.com/codepdbh/nfsu1-recompiled/releases/download/v0.2.5/NFSU1-AndroidEvolved-v0.2.5-arm64.apk) | arm64-v8a, recommended |
| [ARMv7 / 32 bits](https://github.com/codepdbh/nfsu1-recompiled/releases/download/v0.2.5/NFSU1-AndroidEvolved-v0.2.5-armeabi-v7a.apk) | armeabi-v7a; requires Android with 32-bit app support |

Android 10+ and Vulkan 1.1+. Both APKs use the same package and signing key;
install the architecture supported by your device. Original game data is required.
Audio, pacing and validation details: [runtime follow-up](docs/AUDIO_FRAME_PACING.md).
Issue-driven changes and remaining limits: [16 October review](docs/ISSUE_REVIEW_2026_10_16.md).

**[English](#english) · [Español](#español)**

---

## English

A **static recompilation** of *Need for Speed Underground* (2003, PC patch 1.4, `Speed.exe`) into native code, built on the toolchain that already runs **[Need for Speed Underground 2 Recompiled](https://github.com/codepdbh/nfsu2-recompiled)** natively on Windows and on Android ARM64.

The x86 executable is translated function by function into C with a patched [pcrecomp](https://github.com/sp00nznet/pcrecomp) (lift32), then compiled for the target. **No emulator, no Wine, no JIT**: the game's own logic runs as native code, and the graphics go through Direct3D 9 (DXVK on Vulkan on Android).

> **You need your own copy of the game.** This repository contains **no game files and no code generated from the game**. Everything derived from `Speed.exe` is produced locally from your installation and is never committed.

### Goals

- **Windows** (x86) native host as the validation platform.
- **Android ARM64 and ARMv7** APKs with launcher, touch controls, gamepad support, resolution scaling, FPS limiter and native widescreen — like the NFSU2 port.
- A **shared base** for future ports, without merging unrelated recompilation paths (x86 here, PowerPC/Xbox 360 for Most Wanted).

### Status

| Stage | State |
|---|---|
| Executable analysis (`Speed.exe` 1.4, SHA-256 `c0fad450…c074b7`) | ✅ Direct3D 9, 254 imports (87 % shared with NFSU2), no RTTI, no relocations |
| Function catalog | ✅ 22,751 functions; seeds from aligned data pointers and 191 vtables |
| Lift to C | ✅ 0 lift failures, 3 hand-written overrides (CPU probe, CRT memmove) |
| Windows host: build and link | ✅ |
| Sound-bank code (x86 routines inside EA `.abk` banks) | ✅ 23 distinct blocks lifted ahead of time, matched by content at run time |
| Windows host: boot | ✅ intro videos, front end, profile load, story-mode video |
| First frame, menus, races | 🔧 menus reached; races not tested yet |
| Android APK | 🔧 arm64 build installs and reaches the menus |
| Widescreen | ⏳ |

### Layout

```
config/     overrides, excluded entries and hook sites for Speed.exe
docs/       audit of what is shared with the NFSU2 project, analysis notes
scripts/    lift driver, seed generation (vtables, aligned data pointers)
src/        Windows host, runtime, Win32 shims, logging
work/       (local only) analysis copy of Speed.exe, catalog, seeds
src/recomp/gen/  (local only) generated C
src/recomp/gen_abk/  (local only) generated C of the sound-bank routines
```

### Building (Windows)

Requirements: Visual Studio (MSVC x86), Python 3 with `capstone` and `pefile`, and a checkout of the patched `pcrecomp` next to this folder (patches in the NFSU2 repository, `patches/pcrecomp`).

```bash
source scripts/vsenv.sh x86
python scripts/data_seeds.py work/Speed.analysis.exe work/seeds_all.json   # after copying Speed.exe to work/
python scripts/run_lift.py catalog work/Speed.analysis.exe work/seeds_all.json work/catalog.json
python scripts/run_lift.py lift work/Speed.analysis.exe work/catalog.json src/recomp/gen
python scripts/abk_module.py "../Need for Speed Underground" work/abk_analysis.dll work/abk_seeds.json src/recomp/gen_abk/abk_blocks.c
python scripts/run_lift.py catalog work/abk_analysis.dll work/abk_seeds.json work/abk_catalog.json
python scripts/run_lift.py lift work/abk_analysis.dll work/abk_catalog.json src/recomp/gen_abk --prefix abk_
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl
cmake --build build
build/NFSU-Recompiled.exe --windowed
```

The host looks for `Speed.exe` in a `Need for Speed Underground` folder next to it (or pass `--game-root`).

### Credits

- **[pcrecomp](https://github.com/sp00nznet/pcrecomp)** — the static recompiler this project builds on.
- **DXVK** — Direct3D 9 on Vulkan (Android).
- The **NFSU2 Recompiled** project, whose runtime and tooling are reused here.

Need for Speed and Underground are trademarks of Electronic Arts. This is an unofficial, non-commercial preservation project.

---

## Español

Una **recompilación estática** de *Need for Speed Underground* (2003, parche 1.4 de PC, `Speed.exe`) a código nativo, construida sobre las herramientas que ya hacen funcionar **[Need for Speed Underground 2 Recompiled](https://github.com/codepdbh/nfsu2-recompiled)** de forma nativa en Windows y en Android ARM64.

El ejecutable x86 se traduce función a función a C con una versión parcheada de [pcrecomp](https://github.com/sp00nznet/pcrecomp) (lift32) y después se compila para cada plataforma. **Sin emulador, sin Wine, sin JIT**: la lógica del juego se ejecuta como código nativo, y los gráficos pasan por Direct3D 9 (DXVK sobre Vulkan en Android).

> **Necesitas tu propia copia del juego.** Este repositorio **no contiene archivos del juego ni código generado a partir de él**. Todo lo derivado de `Speed.exe` se genera localmente desde tu instalación y nunca se sube.

### Objetivos

- Host nativo de **Windows** (x86) como plataforma de validación.
- APKs de **Android ARM64 y ARMv7** con launcher, controles táctiles, mando, escalado de resolución, limitador de FPS y widescreen nativo, como el port de NFSU2.
- Una **base compartida** para futuros ports, sin mezclar rutas de recompilación distintas (x86 aquí, PowerPC/Xbox 360 para Most Wanted).

### Estado

| Etapa | Estado |
|---|---|
| Análisis del ejecutable (`Speed.exe` 1.4, SHA-256 `c0fad450…c074b7`) | ✅ Direct3D 9, 254 importaciones (87 % compartidas con NFSU2), sin RTTI ni reubicaciones |
| Catálogo de funciones | ✅ 22.751 funciones; semillas de punteros alineados en datos y de 191 vtables |
| Traducción a C | ✅ 0 fallos, 3 reemplazos escritos a mano (detección de CPU, memmove de la CRT) |
| Host de Windows: compilación y enlazado | ✅ |
| Código de los bancos de sonido (rutinas x86 dentro de los `.abk` de EA) | ✅ 23 bloques distintos traducidos de antemano, reconocidos por contenido en ejecución |
| Host de Windows: arranque | ✅ vídeos de introducción, menú principal, carga de perfil, vídeo del modo historia |
| Primer fotograma, menús, carreras | 🔧 llega a los menús; carreras aún sin probar |
| APK de Android | 🔧 la versión arm64 se instala y llega a los menús |
| Widescreen | ⏳ |

### Compilar (Windows)

Requisitos: Visual Studio (MSVC x86), Python 3 con `capstone` y `pefile`, y el `pcrecomp` parcheado junto a esta carpeta (los parches están en el repositorio de NFSU2, `patches/pcrecomp`). Los comandos son los mismos de la sección en inglés.

El host busca `Speed.exe` en una carpeta `Need for Speed Underground` junto a él (o pásala con `--game-root`).

### Créditos

- **[pcrecomp](https://github.com/sp00nznet/pcrecomp)**: el recompilador estático sobre el que se construye este proyecto.
- **DXVK**: Direct3D 9 sobre Vulkan (Android).
- El proyecto **NFSU2 Recompiled**, cuyo runtime y herramientas se reutilizan aquí.

Need for Speed y Underground son marcas de Electronic Arts. Este es un proyecto de preservación no oficial y sin fines comerciales.
