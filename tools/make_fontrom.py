#!/usr/bin/env python3
"""Builds a system font ROM image (the board's "fontrom" partition).

Fonts in the font ROM are read in place from Flash by every app: an app that
does not carry its own font gets (use-font id "face" size) from here, by the
name "<face>-<size>". Layout: minischeme_fontrom.h.

    python tools/make_fontrom.py simsun-16.mfnt kai-20.mfnt -o fontrom.bin
    (firmware/fontrom.bin: minischeme-webrepl/fonts/simsun-16.mfnt, GB2312 levels 1 and 2)
    python tools/make_fontrom.py --font simsun:16:gb2312 --font simhei:24:app.scm -o fontrom.bin
    python -m esptool --chip esp32 -p COM42 write-flash 0x190000 fontrom.bin

A --font spec is face:size[:what], what being gb2312 (all of GB2312), a
source file whose string literals are included, or omitted (ASCII only);
the font is named <face>-<size>. Single fonts can also be added on the board
without reflashing: WebREPL font manager, or (fontrom-add path [name]).
"""
import argparse
import os
import struct
import sys

SECTOR = 4096
NAME = 24
DEFAULT_OFFSET = 0x190000
DEFAULT_SIZE = 768 * 1024


def fnv(data, h=2166136261):
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def mfnt_name(data):
    raw = data[16:32].split(b'\0')[0].decode('utf-8', 'replace')
    return ''.join(c if c.isalnum() or c in '-_.' else '_' for c in raw) or 'font'


def build_image(fonts, size=DEFAULT_SIZE):
    """fonts: [(name, mfnt bytes)] -> image bytes (directory + fonts)."""
    entries, body, offset = [], bytearray(), 2 * SECTOR
    for name, data in fonts:
        if data[:4] != b'MFNT':
            sys.exit(f'make_fontrom: {name} is not an MFNT font')
        if not 0 < len(name) < NAME:
            sys.exit(f'make_fontrom: font name must be 1..{NAME - 1} characters: {name}')
        if any(n == name for n, _, _ in entries):
            sys.exit(f'make_fontrom: duplicate font name {name}')
        entries.append((name, offset, len(data)))
        body += data
        pad = -len(data) % SECTOR
        body += b'\xff' * pad
        offset += len(data) + pad
    if offset > size:
        sys.exit(f'make_fontrom: {offset} bytes do not fit the {size} byte partition')
    if len(entries) > 32:
        sys.exit('make_fontrom: at most 32 fonts')
    table = b''.join(n.encode('utf-8').ljust(NAME, b'\0') + struct.pack('<II', o, ln) for n, o, ln in entries)
    head = struct.pack('<HHI', 1, len(entries), 1)
    directory = b'MFRM' + head + struct.pack('<I', fnv(table, fnv(head))) + table
    # sector 1 erased: the other directory copy must not look newer
    return directory.ljust(SECTOR, b'\xff') + b'\xff' * SECTOR + bytes(body)


def parse_size(text):
    t = text.upper()
    if t.endswith('K'):
        return int(t[:-1]) * 1024
    if t.endswith('M'):
        return int(t[:-1]) * 1024 * 1024
    return int(text, 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('mfnt', nargs='*', help='.mfnt files (named by the name stored in them)')
    ap.add_argument('--font', action='append', default=[], help='face:size[:gb2312|source-file] rendered here')
    ap.add_argument('--size', default='512K', help='partition size (default 512K, firmware 1.0.71+)')
    ap.add_argument('-o', '--output', required=True)
    a = ap.parse_args()
    fonts = []
    for path in a.mfnt:
        with open(path, 'rb') as f:
            data = f.read()
        fonts.append((mfnt_name(data), data))
    for spec in a.font:
        # rendering from TTF needs make_font.py (minischeme-webrepl/tools) and Pillow
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import make_font
        parts = spec.split(':', 2)
        if len(parts) < 2:
            sys.exit(f'make_fontrom: bad --font {spec}')
        face, size = parts[0], int(parts[1])
        what = parts[2] if len(parts) > 2 else ''
        chars = make_font.gb2312_chars() if what == 'gb2312' else (make_font.string_chars([what]) if what else set())
        name = f'{os.path.splitext(os.path.basename(face))[0]}-{size}'
        fonts.append((name, make_font.build_mfnt(face, size, chars, name)))
    if not fonts:
        sys.exit('make_fontrom: no fonts given')
    image = build_image(fonts, parse_size(a.size))
    with open(a.output, 'wb') as f:
        f.write(image)
    for name, data in fonts:
        print(f'  {name:<24} {len(data):>8} bytes, {struct.unpack_from("<I", data, 8)[0]} glyphs')
    print(f'{a.output}: {len(image)} bytes; flash with\n'
          f'  python -m esptool --chip esp32 -p PORT write-flash 0x{DEFAULT_OFFSET:x} {a.output}')


if __name__ == '__main__':
    main()
