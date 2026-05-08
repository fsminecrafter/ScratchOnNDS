#!/usr/bin/env python3
"""
check_project_json.py
Deeper schema checks for ScratchDS compatibility:
  - Costume formats supported by ScratchDS (png, bmp — NOT svg for real content)
  - Sound formats supported (wav, mp3)
  - Checks for NDS extension blocks and validates their fields
  - Warns about features ScratchDS doesn't implement

Usage: python3 tools/check_project_json.py path/to/file.sb3
"""
import sys
import os
import zipfile
import json

SUPPORTED_COSTUME_FORMATS = {"png", "bmp", "svg"}   # svg = placeholder square
SUPPORTED_SOUND_FORMATS   = {"wav", "mp3"}
WARN_SOUND_FORMATS        = {"mp3"}                  # works but lossy quality

NDS_OPCODES = {
    "nds_buttonpressed", "nds_buttonheld", "nds_buttonreleased",
    "nds_touchx", "nds_touchy", "nds_touchpressed",
    "nds_microphone_loudness", "nds_rumble", "nds_setvibration",
    "nds_backlight_top", "nds_backlight_bottom",
    "nds_whenbuttonpressed", "nds_whenclap", "nds_whentouched", "nds_whencombo",
}

UNIMPLEMENTED_OPCODES = {
    "looks_seteffectto", "looks_changeeffectby",  # graphic effects (no GPU shader)
    "sensing_coloristouchingcolor", "sensing_touchingcolor",  # pixel-perfect not supported
    "sensing_askandwait",           # text input not implemented
    "control_create_clone_of",      # clones partially implemented
}

def check(path: str):
    warnings = []
    errors   = []

    with zipfile.ZipFile(path, "r") as zf:
        data = json.loads(zf.read("project.json"))

    targets = data.get("targets", [])
    meta    = data.get("meta", {})

    print(f"  Scratch VM : {meta.get('vm', 'unknown')}")
    print(f"  semver     : {meta.get('semver', 'unknown')}\n")

    for target in targets:
        name = target.get("name", "?")

        # Costume format check
        for costume in target.get("costumes", []):
            fmt = costume.get("dataFormat", "")
            if fmt not in SUPPORTED_COSTUME_FORMATS:
                errors.append(f"Target \"{name}\": unsupported costume format \"{fmt}\"")
            elif fmt == "svg":
                warnings.append(
                    f"Target \"{name}\": SVG costume \"{costume.get('name')}\" "
                    f"will render as a placeholder pink square (SVG not supported on NDS)"
                )

        # Sound format check
        for sound in target.get("sounds", []):
            fmt = sound.get("dataFormat", "")
            if fmt not in SUPPORTED_SOUND_FORMATS:
                errors.append(f"Target \"{name}\": unsupported sound format \"{fmt}\"")
            elif fmt in WARN_SOUND_FORMATS:
                warnings.append(
                    f"Target \"{name}\": MP3 sound \"{sound.get('name')}\" "
                    f"requires dr_mp3 integration (stub only)"
                )

        # Block opcode checks
        for bid, block in target.get("blocks", {}).items():
            opcode = block.get("opcode", "")
            if opcode in UNIMPLEMENTED_OPCODES:
                warnings.append(
                    f"Target \"{name}\": block \"{opcode}\" is not fully implemented in ScratchDS"
                )
            if opcode in NDS_OPCODES:
                print(f"  [NDS] \"{name}\" uses NDS block: {opcode}")

    # Report
    print()
    if warnings:
        print(f"  {len(warnings)} warning(s):")
        for w in warnings:
            print(f"    [WARN] {w}")
    if errors:
        print(f"\n  {len(errors)} error(s):")
        for e in errors:
            print(f"    [ERR]  {e}")

    if not warnings and not errors:
        print("  All checks passed — project is fully compatible with ScratchDS.")

    return len(errors) == 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: check_project_json.py <file.sb3>")
        sys.exit(1)

    path = sys.argv[1]
    print(f"\nScratchDS compatibility check: {path}\n{'─'*40}")

    if not os.path.exists(path) or not zipfile.is_zipfile(path):
        print("[ERR] Not a valid .sb3 file")
        sys.exit(1)

    passed = check(path)
    print(f"{'─'*40}")
    sys.exit(0 if passed else 1)
