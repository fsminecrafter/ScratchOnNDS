#!/usr/bin/env python3
"""
validate_sb3.py
Validates that a .sb3 file:
  1. Is a valid ZIP archive
  2. Contains project.json
  3. project.json parses as valid JSON
  4. Has at least one target (sprite/stage)
  5. Each target has the required fields

Usage: python3 tools/validate_sb3.py path/to/file.sb3
"""
import sys
import os
import zipfile
import json

REQUIRED_TARGET_FIELDS = ["isStage", "name", "costumes", "blocks", "variables"]

def validate(path: str) -> bool:
    ok = True

    # 1. File exists
    if not os.path.exists(path):
        print(f"[FAIL] File not found: {path}")
        return False
    print(f"[OK]   Found: {path} ({os.path.getsize(path)} bytes)")

    # 2. Valid ZIP
    if not zipfile.is_zipfile(path):
        print(f"[FAIL] Not a valid ZIP/sb3 archive")
        return False
    print("[OK]   Valid ZIP archive")

    with zipfile.ZipFile(path, "r") as zf:
        names = zf.namelist()

        # 3. Contains project.json
        if "project.json" not in names:
            print("[FAIL] Missing project.json")
            return False
        print("[OK]   project.json present")

        # 4. project.json parses
        try:
            data = json.loads(zf.read("project.json"))
        except json.JSONDecodeError as e:
            print(f"[FAIL] project.json is invalid JSON: {e}")
            return False
        print("[OK]   project.json is valid JSON")

        # 5. Has targets
        targets = data.get("targets", [])
        if not targets:
            print("[FAIL] No targets in project.json")
            return False
        print(f"[OK]   {len(targets)} target(s) found")

        # 6. At least one stage
        stages = [t for t in targets if t.get("isStage")]
        if not stages:
            print("[FAIL] No stage target found")
            ok = False
        else:
            print(f"[OK]   Stage target: \"{stages[0].get('name', '?')}\"")

        # 7. Required fields on each target
        for i, target in enumerate(targets):
            name = target.get("name", f"target[{i}]")
            for field in REQUIRED_TARGET_FIELDS:
                if field not in target:
                    print(f"[FAIL] Target \"{name}\" missing field: {field}")
                    ok = False

        # 8. All costume assetIds have corresponding files
        missing_assets = []
        for target in targets:
            for costume in target.get("costumes", []):
                asset_id = costume.get("assetId", "")
                fmt      = costume.get("dataFormat", "")
                filename = f"{asset_id}.{fmt}"
                if filename not in names:
                    missing_assets.append(filename)
        if missing_assets:
            for a in missing_assets[:5]:  # cap output
                print(f"[WARN] Missing costume asset: {a}")
        else:
            print("[OK]   All costume assets present")

        # 9. Print summary
        sprites = [t for t in targets if not t.get("isStage")]
        sounds  = sum(len(t.get("sounds", [])) for t in targets)
        print(f"\n  Targets : {len(targets)} ({len(sprites)} sprites, {len(stages)} stage)")
        print(f"  Sounds  : {sounds}")
        print(f"  Assets  : {len([n for n in names if '.' in n and not n.endswith('.json')])}")

    return ok


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: validate_sb3.py <file.sb3>")
        sys.exit(1)

    path = sys.argv[1]
    print(f"\nValidating: {path}\n{'─'*40}")
    passed = validate(path)
    print(f"\n{'─'*40}")
    if passed:
        print("PASS — sb3 file is valid")
        sys.exit(0)
    else:
        print("FAIL — sb3 file has errors")
        sys.exit(1)
