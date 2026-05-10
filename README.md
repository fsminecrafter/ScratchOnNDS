# ScratchOnNDS

A Scratch 3.0 runtime for the Nintendo DS. Place `.sb3` projects on your R4 SD card and run them on real hardware.

Quick command.

```
rm -rf ScratchOnNDS && git clone https://github.com/fsminecrafter/ScratchOnNDS.git && cd ScratchOnNDS && make
```

---

## SD Card Layout

Copy `ScratchDS.nds` to your R4 card root. Then create the following structure under `fat:/`:

```
fat:/
└── scratch/                      ← root project directory
    │
    ├── example/                  ← fallback project (loaded if nothing else found)
    │   └── example.sb3
    │
    ├── MyProject.sb3             ← your Scratch projects go here
    ├── AnotherGame.sb3
    ├── PlatformerDemo.sb3
    │
    ├── .tmp/                     ← auto-created; extraction cache (do not edit)
    │
    ├── out/                      ← compiled .sds output (written by overlay menu)
    │
    └── .settings                 ← auto-saved settings (FPS, screen layout, last project)
```

---

## Getting Started

1. Flash `ScratchDS.nds` to your R4 card.
2. Create `fat:/scratch/` on the SD card.
3. Drop any `.sb3` files into `fat:/scratch/`.
4. Optionally, place a fallback project at `fat:/scratch/example/example.sb3`.
5. Boot your DS.

**On first boot**, the runtime scans `fat:/scratch/` for `.sb3` files. If more than one is found, a file selector appears (D-pad to navigate, A to load). If none are found, the fallback project at `fat:/scratch/example/example.sb3` is loaded instead. If that is also missing, a waiting screen is shown — hold **L+R+B** for ~0.4 seconds to open the overlay menu and browse for a project.

**On subsequent boots**, the last-loaded project path is stored in `fat:/scratch/.settings` and resumed automatically, skipping the selector.

---

## Project Compatibility

Export your project from Scratch as an `.sb3` file. Use the included tools to check compatibility before copying to the SD card:

```sh
python3 tools/validate_sb3.py path/to/project.sb3
python3 tools/check_project_json.py path/to/project.sb3
```

| Feature | Support |
|---|---|
| Costumes | PNG, BMP. SVG renders as a placeholder pink square. |
| Audio | WAV (full), MP3 (stub only — convert to WAV for reliable audio) |
| Max sprite size | 64×64 px (clamped) |
| OAM sprite limit | 128 per frame |
| Large sounds | Streamed from SD when file size exceeds 2 MB |
| Graphic effects | Not supported (`looks_seteffectto`, `looks_changeeffectby`) |
| Pixel collision | Not supported (`sensing_coloristouchingcolor`) |
| Ask and wait | Not supported (`sensing_askandwait`) |

---

## NDS Hardware Extension Blocks

Projects can use NDS-specific Scratch blocks by loading the companion editor plugin (`source/scratch_extension/nds_extension_blocks.js`) into a TurboWarp or custom scratch-vm fork.

Available block categories:

- **NDS Input** — button pressed / held / released, D-pad, combo detection (e.g. L+R, A+B)
- **NDS Touch** — touchscreen X/Y position, drag delta, tap detection
- **NDS Microphone** — loudness (0–100), clap detection, recording state
- **NDS Triggers** — hat blocks: "when button pressed", "when clap detected", "when combo held"
- **NDS System** — top/bottom backlight on/off, rumble pak pulse, rumble on/off

The overlay menu combo (**L+R+B** held for ~0.4s) is reserved and cannot be remapped.

---

## Overlay Menu

Hold **L + R + B** simultaneously for ~0.4 seconds at any time to pause the Scratch VM and open the system overlay. The menu provides:

- **Info** — build version, device model, free RAM, current project name
- **Settings** — target FPS (30/60), screen layout (stage top vs bottom), stage scale mode (stretch/aspect/native), FPS counter toggle, compile pass
- **Load** — SD card file browser for `.sb3` files; navigate with D-pad, A to select, B to go up
- **Resume** — return to the running project

Settings are persisted to `fat:/scratch/.settings` automatically.

---

## Building from Source

**Requirements:** devkitARM r62+, libnds 2.x, libfat, maxmod.

```sh
export DEVKITPRO=/opt/devkitpro
make                  # produces ScratchDS.nds
make version          # print version and toolchain info
make clean
```

**Host-side unit tests** (no DS hardware or devkitARM required):

```sh
cmake -B build -DSCRATCHDS_HOST_TESTS=ON
cmake --build build
ctest --test-dir build
```

Test suites cover: `ScratchValue` type coercion, project JSON parsing, VM operator execution, opcode string mapping, clap detector state machine, and combo tracker logic.

---

## License

GNU General Public License v3.0. See [LICENSE](LICENSE) for the full text.
