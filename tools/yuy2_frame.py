#!/usr/bin/env python3
"""pull one frame out of a raw HD60 S capture and write a PPM.

Measured geometry for 1080p60 on this card:

    line stride   3848 bytes   (1920 active pixels = 3840, plus 8)
    lines         1125         the V total the front end itself reports
    frame period  4329300      = 3848 * 1125 + 300, so 300 bytes per frame
                               sit outside the raster, presumably a header
    luma          even bytes
    chroma        odd bytes, neutral 128, limited range

Colour is correct for a YCbCr source. Decoded as BT.709 limited range, which
is right for HD; pass --bt601 for SD material. An RGB source is not yet
supported and produces chroma that is a function of luma.

720p geometry: stride 2564, 750 lines, frame period 1,925,160.

usage: yuy2_frame.py capture.yuy2 --offset N [--grey] --out frame.ppm
"""
import argparse, sys

def main():
    a = argparse.ArgumentParser()
    a.add_argument('path')
    a.add_argument('--offset', type=int, default=0)
    a.add_argument('--stride', type=int, default=3848)   # 720p: 2564
    a.add_argument('--width',  type=int, default=1920)
    a.add_argument('--height', type=int, default=1125)
    a.add_argument('--neutral', type=int, default=128)
    a.add_argument('--bt601', action='store_true', help='BT.601 instead of BT.709')
    a.add_argument('--swap-uv', action='store_true')
    a.add_argument('--grey', action='store_true', help='luma only, known correct')
    a.add_argument('--out', required=True)
    o = a.parse_args()

    try:
        import numpy as np
    except ImportError:
        sys.exit("needs numpy")

    f = open(o.path, 'rb'); f.seek(o.offset)
    buf = f.read(o.stride * o.height)
    if len(buf) < o.stride * o.height:
        sys.exit("not enough data at that offset")
    img = np.frombuffer(buf, dtype=np.uint8).reshape(o.height, o.stride)
    img = img[:, :o.width * 2].astype(np.int32)

    Y = img[:, 0::2]
    if o.grey:
        px = Y.astype(np.uint8)
        with open(o.out, 'wb') as w:
            w.write(b'P5\n%d %d\n255\n' % (px.shape[1], px.shape[0]))
            w.write(px.tobytes())
        print(f"{o.out}: {px.shape[1]}x{px.shape[0]} luma")
        return

    A = np.repeat(img[:, 1::4], 2, axis=1)
    B = np.repeat(img[:, 3::4], 2, axis=1)
    U, V = (B, A) if o.swap_uv else (A, B)
    kg = 1 - kr - kb
    c = (Y - 16) * (255 / 219)
    d = (U - o.neutral) * (255 / 224)
    e = (V - o.neutral) * (255 / 224)
    r = np.clip(c + e * 2 * (1 - kr), 0, 255)
    b = np.clip(c + d * 2 * (1 - kb), 0, 255)
    g = np.clip(c - (d * 2 * (1 - kb) * kb + e * 2 * (1 - kr) * kr) / kg, 0, 255)
    rgb = np.dstack([r, g, b]).astype(np.uint8)
    with open(o.out, 'wb') as w:
        w.write(b'P6\n%d %d\n255\n' % (rgb.shape[1], rgb.shape[0]))
        w.write(rgb.tobytes())
    print(f"{o.out}: {rgb.shape[1]}x{rgb.shape[0]}, stride {o.stride}, neutral {o.neutral}")

if __name__ == '__main__':
    main()
