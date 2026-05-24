#!/usr/bin/env python3
"""Convert one of mac68k_host's 512×342 framebuffer BMPs to a PNG with a
red coordinate-grid overlay every 50 pixels. Used by the MacBench /
THINK C bench-target scripting to make it easy to identify icon
positions visually.

Usage: scripts/bmp2png_grid.py <in.bmp> <out.png> [grid_step=50]

No external dependencies (pure stdlib zlib/struct PNG writer)."""
import struct, sys, zlib


def main():
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__)
        sys.exit(1)
    in_path  = sys.argv[1]
    out_path = sys.argv[2]
    step     = int(sys.argv[3]) if len(sys.argv) > 3 else 50

    with open(in_path, 'rb') as f:
        data = f.read()
    data_offset = int.from_bytes(data[10:14], 'little')
    w = int.from_bytes(data[18:22], 'little')
    h = int.from_bytes(data[22:26], 'little')

    rgb_lines = []
    for y in range(h):
        line = bytearray([0])  # PNG filter byte
        for x in range(w):
            # BMP rows are bottom-up; the mac68k_host writer uses
            # 1bpp-as-8bpp (0=black, 255=white) at offset 0x436.
            v = data[data_offset + (h - 1 - y) * w + x]
            if x % step == 0 or y % step == 0:
                line.extend([255, 0, 0])
            else:
                line.extend([0, 0, 0] if v < 128 else [255, 255, 255])
        rgb_lines.append(bytes(line))

    def png_chunk(typ, payload):
        crc = zlib.crc32(typ + payload)
        return struct.pack('>I', len(payload)) + typ + payload + struct.pack('>I', crc)

    ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)  # 8bpc RGB
    idat = zlib.compress(b''.join(rgb_lines), 9)
    png = (
        b'\x89PNG\r\n\x1a\n'
        + png_chunk(b'IHDR', ihdr)
        + png_chunk(b'IDAT', idat)
        + png_chunk(b'IEND', b'')
    )
    with open(out_path, 'wb') as f:
        f.write(png)
    print(f'saved {len(png)} bytes to {out_path}')


if __name__ == '__main__':
    main()
