# Sonic Dash — Nintendo Switch port (Unity 6 / IL2CPP wrapper)
 
This is a native wrapper / loader that runs the original ARM64 Android build of Sonic Dash v10.3.1 on Switch homebrew. It contains no game code and no game assets — it loads the game's own libraries and recreates, natively, the Android layer Unity expects: the activity and its window, EGL, input, storage, and the JNI surface the game and SEGA's SDKs call out to. Ads, purchases and sign-in report unavailable, so the game runs its own offline path.
 
## Install & run
 
You need files from your own copy of Sonic Dash v10.3.1. It ships as an App Bundle, so that is three APKs.
```
 
```
sdmc:/switch/sonicdash_nx
├── sonicdash_nx.nro
├── libmain.so              <- from config.arm64_v8a.apk: lib/arm64-v8a/
├── libunity.so                (with lib_burst_generated.so and
├── libil2cpp.so                libgraphics-core.so)
├── assets/bin/Data/        <- from the base APK and the asset pack, merged
├── assets/aa/Android/      <- from UnityDataAssetPack.apk: 292 bundles. Required:
│                              without them the game cannot reach a scene
├── config.txt              (written on first launch)
└── save_edit.txt           (written on first launch)
```
 
Launch over a game — hold R while starting any installed title — not from the album: Unity needs more memory than the album applet gets.
 
## Controls
 
Sonic Dash is played by touch, so the controller drives an on-screen cursor.
 
| Input | Action |
|---|---|
| Touchscreen | Tap and swipe (handheld) |
| + | Toggle the on-screen cursor |
| – | Toggle gyro pointing (tilt/turn the controller to aim) |
| Left stick | Move the cursor |
| L / R | Recenter the cursor (helps gyro aiming) |
| A / ZR / ZL | Tap / confirm (ZL and ZR let you play one-handed) |
| D-pad up / down | Adjust sensitivity of whatever is driving the cursor |
 
The cursor is on by default docked and off in handheld; + overrides either way. A USB mouse works in both modes: click to tap, scroll to change sensitivity, and gyro turns itself off while one is connected. Sensitivities are saved in `pointer.cfg`; a `cursor.png` of up to 64×64 next to the `.nro` replaces the arrow.
 
## Settings
 
`config.txt` is written next to the `.nro` on first launch, documented inline:
 
```
resolution = 720    # short side, 720 .. 1080 — the same docked and handheld
rotation = 0        # 0 none, 1 = 90 clockwise, 2 = 90 counter-clockwise
language = auto     # auto, or en fr de it pt ru es
```
 
`auto` follows the Switch's language when the game has it, and English otherwise. Rotation is for holding the console upright: the game switches to its phone-style portrait layout.
 
## Save editing
 
`save_edit.txt` is written next to the `.nro` on first launch, every line commented out. Uncomment a line and it is applied at each launch; the save is re-signed with the game's own checksum, and the untouched original is kept once as `save.txt.orig`.

 ## Building
 
Requires devkitPro with the switch-dev group plus these portlibs:
 
```
dkp-pacman -S switch-dev
dkp-pacman -S switch-sdl2 switch-mesa switch-libdrm_nouveau switch-zlib \
              switch-libpng
 
export DEVKITPRO=/opt/devkitpro
make                        # -> sonicdash_nx.nro
```
 
## Credits
 
The loader and shim infrastructure — so_util, libc_shim, jni_fake and the diagnostics — derives from the open-source Switch/Vita .so-loader lineage: Andy Nguyen, fgsfds and Rinnegatamante, building on TheOfficialFloW's loader tradition. It reaches this project via the Zookeeper DX, CloverPit, Fruit Ninja and Colour Sheep ports; the cursor, rotation and `config.txt` come from the Bloons Pop port, the save editor's design from Bloons Adventure Time TD, and the window-geometry rule and Java callbacks from Data Defense. All MIT-licensed. Thanks to everyone in that lineage for making this approach possible.
