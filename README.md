# AlphaHaloQuest

An experimental native Quest port of the original Xbox version of *Halo:
Combat Evolved*. It runs on the headset with OpenXR and OpenGL ES. It does not
use a PC, Link, Winlator, Wine, or SteamVR.

The blunt description: this is Halo CE Universal and a VR mod mashed together,
built with lots of help from LLMs. It is early alpha software and is potentially
very broken. It is janky, buggy, and does not work right in plenty of places—but
it is already fun. Expect crashes, bad controls, visual problems, unfinished VR
behavior, and regressions between builds. Keep a backup of anything you care
about.

This repository is based on
[halo-ce-universal](https://github.com/cybersecurity/halo-ce-universal), which
in turn builds on the Xbox Halo decompilation by
[bnunu/halo-1](https://github.com/bnunu/halo-1) and
[punpckhdq/halo](https://github.com/punpckhdq/halo).

It is suitable for testing, not a finished release.

## **Quick start**

1. **[Download and install the latest AlphaHaloQuest APK](https://github.com/christiancoler/AlphaHaloQuest/releases/latest/download/AlphaHaloQuest.apk).**
2. Open AlphaHaloQuest and choose **Import Xbox disc image**.
3. Select your legally dumped original-Xbox Halo CE `.iso` or `.xiso`, wait
   for the import to finish, and start the game.

The APK contains no Halo game data. Installing an update over an existing
AlphaHaloQuest installation preserves imported files and saves; uninstalling
the app removes them.

## Current state

Working now:

- native ARM64 Android build and standalone OpenXR session;
- separate tracked views for both eyes;
- Quest Touch buttons, sticks, triggers, grips, poses, and haptics;
- controller-directed weapon and grenade aiming;
- tracked first-person weapon model;
- experimental two-hand weapon support;
- physical melee, flashlight, and crouch gestures;
- a tracked weapon reticle outside weapon zoom;
- main menu and pause screens presented to both eyes;
- direct import from an original-Xbox `.iso`/`.xiso` on the headset.

Still missing or incomplete:

- polished hand and weapon animation;
- reliable aiming and two-hand behavior in every situation;
- a visible reticle while weapon zoom is active;
- VR-native HUD placement and scopes;
- left-handed weapon support;
- vehicle controls designed for VR;
- configurable Touch bindings;
- broad testing across the campaign and multiplayer.

## Game data

The APK does not contain Halo maps, audio, movies, or other retail assets.

On first launch, select a legally dumped original-Xbox Halo CE disc image. The
app extracts the required files directly. It also accepts an extracted folder
containing `maps/ui.map`.

If your original disc has gone missing, check wherever you normally look for
lost things or obtain a legitimate replacement. This project cannot provide or
link to copyrighted game data.

Imported files are stored in the app's external files directory. Installing a
new APK over the old one keeps them. Uninstalling the app removes them, so do
not uninstall between test builds.

## Install

Download `AlphaHaloQuest.zip` from the GitHub Releases page, extract the APK,
and install it with [SideQuest](https://sidequestvr.com/) or ADB:

```sh
adb install -r AlphaHaloQuest.apk
```

The Android package name is `com.halo.questvr`, so it can coexist with the
flat Android port.

## Controls

The app shows this list before starting the VR session.

| Input | Action |
| --- | --- |
| Left stick | Move |
| Left stick click | Crouch |
| Right stick | Look / snap turn |
| Right stick click | Zoom |
| A | Jump / accept |
| B | Melee / back |
| X | Use / reload |
| Y | Change weapon |
| Right trigger | Fire |
| Left trigger | Throw grenade |
| Left grip | Hold a two-handed weapon / support-hand interaction |
| Right grip | Change grenade type |
| Left menu button | Pause |

Hold the left grip while placing the off-hand on a long weapon to engage
two-hand aiming; release it to return to one hand. Pistol bracing is
experimental and may snap visually when the hands are close, but pistol aim
still follows the firing controller. Lowering the headset about 15 cm also
crouches. Moving the off-hand to the headset toggles the flashlight. A fast
vertical controller swing triggers melee.

## Known rough edges

- Weapon zoom currently has no visible reticle.
- Two-hand aiming can feel awkward and does not automatically attach to the
  assault rifle by proximity; hold the left grip.
- Weapon, arm, reload, and melee animations are unfinished and sometimes look
  completely wrong.
- The aiming ray, weapon model, and reticle can still disagree.
- HUD elements are adapted from a flat 4:3 interface and can be misplaced or
  uncomfortable in stereo.
- Movement direction and snap turning need more headset testing.
- Campaign progression, vehicles, cinematics, menus, and checkpoints have not
  been comprehensively tested.
- Lots of general jank
- Probably a ton of undiscovered stuff, as at this point I've mostly only been playing the first few minutes of the Halo level for testing

## Resolution

Quest 2 defaults to a 1200×1200 render target per eye. Quest 3, Quest 3S, and
Quest Pro use 1920×1920. These defaults leave memory headroom while campaign
maps load. They are internal render sizes;
OpenXR still presents through the headset's normal eye buffers.

If performance is poor, edit `config.toml` in the app data directory:

```toml
[vr]
auto_render_scale = false
render_scale = 3.0
```

`3.0` renders 1440×1440 per eye. `2.0` renders 960×960.

## Build

The build needs Python, Ninja, JDK 17, Android SDK 35, and an Android NDK. The
Android build also downloads SDL3, musl, and the Khronos OpenXR loader.

From the repository root:

```sh
python3 configure.py --portable --lto=off --pgo=off
ninja android
cd port/android
./gradlew assembleDebug
```

The APK is written to:

```text
port/android/app/build/outputs/apk/debug/app-debug.apk
```

The existing GitHub Actions workflow builds Android automatically. Add a
keystore as the `ANDROID_KEYSTORE_BASE64` secret and its password as
`ANDROID_KEYSTORE_PASSWORD` if release APKs must install over one another.
Without those secrets, each CI environment uses its own debug signing key.

## HaloCEVR

[HaloCEVR](https://github.com/LivingFray/HaloCEVR), the VR mod for the Halo CE PC port, informed this project's
feature set, control ideas, and several default thresholds for
the Quest implementation. That being said, per the creator's wishes to keep the mod VR only, this contains no HaloCEVR source, assets, etc. it was referenced during creation, but not recreated or forked.

This repository does not include HaloCEVR source files, binaries, shaders, or
assets. The Quest code is a separate OpenXR implementation built into the
decompiled Xbox engine; it does not use HaloCEVR's Direct3D hooks or SteamVR
runtime. This was not a clean-room implementation, because HaloCEVR's source
was read during development.

HaloCEVR's README asks that the mod remain on PC. This project is independent and entirely unrelated

## Licensing and trademarks

The inherited source tree is released under CC0; see [LICENSE.md](LICENSE.md).
Third-party components keep their own licenses in their source directories.
The Khronos OpenXR headers are Apache-2.0 licensed.

No Microsoft or Halo
retail content is distributed here.
