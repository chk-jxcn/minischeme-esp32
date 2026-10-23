#!/usr/bin/env python3
"""make_manifest.py DIR VERSION IDF: writes DIR/firmware-manifest.json for the
three images in DIR (bootloader, partition table, application) with their
flash offsets, sizes and SHA-256, so a flasher can check it got one complete,
intact set (the WebREPL refuses files that do not match). A fontrom.bin in DIR
(tools/make_fontrom.py) is listed too, marked optional: it replaces the whole
font ROM partition, so flashers write it only when asked."""
import hashlib
import json
import os
import sys

FILES = [("bootloader.bin", 0x1000), ("partition-table.bin", 0x8000), ("minischeme_esp32.bin", 0x10000)]
FONT_ROM = ("fontrom.bin", 0x181000)   # partitions.csv: fontrom, 320 KB (0x190000 / 512 KB in 1.0.71..1.0.72)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    folder, version, idf = sys.argv[1], sys.argv[2].lstrip("v"), sys.argv[3]
    files = []
    for name, offset in FILES:
        data = open(os.path.join(folder, name), "rb").read()
        files.append({"name": name, "address": hex(offset), "offset": offset,
                      "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    font_path = os.path.join(folder, FONT_ROM[0])
    if os.path.exists(font_path):
        data = open(font_path, "rb").read()
        files.append({"name": FONT_ROM[0], "address": hex(FONT_ROM[1]), "offset": FONT_ROM[1],
                      "size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "optional": True,
                      "description": "system font ROM: simsun-16, GB2312 levels 1 and 2"})
    manifest = {"version": version, "chip": "ESP32", "idf": idf, "files": files}
    with open(os.path.join(folder, "firmware-manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"{folder}/firmware-manifest.json: v{version}, IDF {idf}")


if __name__ == "__main__":
    main()
