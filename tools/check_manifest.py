#!/usr/bin/env python3
"""check_manifest.py DIR [TAG]: fails unless DIR/firmware-manifest.json lists
the three images with their real sizes and SHA-256 (and, given a tag such as
v1.0.53, names version 1.0.53): the firmware/ folder is a complete release."""
import hashlib
import json
import os
import sys


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    folder = sys.argv[1]
    tag = sys.argv[2].lstrip("v") if len(sys.argv) == 3 else ""
    manifest = json.load(open(os.path.join(folder, "firmware-manifest.json"), encoding="utf-8"))
    errors = []
    if tag and manifest.get("version") != tag:
        errors.append(f"manifest version {manifest.get('version')} is not the tag's {tag}")
    names = {f["name"] for f in manifest.get("files", [])}
    for need in ("bootloader.bin", "partition-table.bin", "minischeme_esp32.bin"):
        if need not in names:
            errors.append(f"{need} missing from the manifest")
    for f in manifest.get("files", []):
        path = os.path.join(folder, f["name"])
        if not os.path.exists(path):
            errors.append(f"{f['name']}: no such file")
            continue
        data = open(path, "rb").read()
        if f.get("size") != len(data):
            errors.append(f"{f['name']}: size {len(data)}, manifest {f.get('size')}")
        if f.get("sha256") != hashlib.sha256(data).hexdigest():
            errors.append(f"{f['name']}: SHA-256 differs from the manifest")
    if errors:
        sys.exit("firmware manifest check failed:\n  " + "\n  ".join(errors))
    print(f"{folder}: firmware v{manifest['version']} (IDF {manifest.get('idf', '?')}), {len(names)} images verified")


if __name__ == "__main__":
    main()
