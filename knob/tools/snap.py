#!/usr/bin/env python3
"""Grab the knob's screen over USB serial and save it as a PNG (masked to the round display).

usage: snap.py OUT.png [--toggle-alert]
"""
import struct
import sys
import time

import serial
from PIL import Image, ImageDraw

out = sys.argv[1]
s = serial.Serial("/dev/ttyACM0", 115200, timeout=3)
if "--toggle-alert" in sys.argv:
    s.write(b"T")
    time.sleep(0.4)
s.reset_input_buffer()
s.write(b"S")

buf = b""
deadline = time.time() + 10
while b"SNAP" not in buf:
    buf += s.read(64)
    if time.time() > deadline:
        sys.exit("no snapshot header")
buf = buf[buf.index(b"SNAP") + 4:]
while len(buf) < 4:
    buf += s.read(4)
w, h = struct.unpack("<HH", buf[:4])
data = buf[4:]
need = w * h * 2
while len(data) < need:
    chunk = s.read(need - len(data))
    if not chunk:
        sys.exit(f"short read: {len(data)}/{need}")
    data += chunk
data = data[:need]

img = Image.new("RGB", (w, h))
px = img.load()
for i in range(w * h):
    v = (data[2 * i] << 8) | data[2 * i + 1]  # LV_COLOR_16_SWAP: high byte first
    px[i % w, i // w] = ((v >> 11) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3)

mask = Image.new("L", (w, h), 0)
ImageDraw.Draw(mask).ellipse((0, 0, w - 1, h - 1), fill=255)
bg = Image.new("RGB", (w, h), (40, 40, 40))
bg.paste(img, (0, 0), mask)
bg.save(out)
print(f"saved {out} ({w}x{h})")
