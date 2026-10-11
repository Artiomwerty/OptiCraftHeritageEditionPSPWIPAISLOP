#!/usr/bin/env python3
"""Generates the XMB art for the PSP EBOOT: assets/psp/ICON0.PNG (144x80) and PIC1.PNG (480x272)."""
import glob, os, random
from PIL import Image, ImageDraw, ImageFont

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "psp")
FONT = (glob.glob("/usr/share/fonts/**/DejaVuSans-Bold.ttf", recursive=True) or [None])[0]


def sky(w, h, rnd):
    im = Image.new("RGBA", (w, h)); px = im.load()
    for y in range(h):
        t = y / h
        c = (int(70 + 110 * t), int(130 + 90 * t), int(235 + 10 * t), 255)
        for x in range(w): px[x, y] = c
    d = ImageDraw.Draw(im)
    for _ in range(max(3, w // 40)):
        x = rnd.randint(0, w - 12); y = rnd.randint(2, h // 2)
        d.rectangle([x, y, x + rnd.randint(8, 16), y + 2], fill=(255, 255, 255, 210))
    return im


def block(img, cx, cy, s, rnd):
    w, h = s, s // 2
    def noisy(poly, base, var):
        m = Image.new("L", img.size, 0); ImageDraw.Draw(m).polygon(poly, fill=255)
        px = img.load(); mp = m.load()
        for y in range(img.size[1]):
            for x in range(img.size[0]):
                if mp[x, y]:
                    v = rnd.randint(-var, var)
                    px[x, y] = tuple(max(0, min(255, c + v)) for c in base) + (255,)
    top = [(cx, cy - h), (cx + w, cy), (cx, cy + h), (cx - w, cy)]
    left = [(cx - w, cy), (cx, cy + h), (cx, cy + h + s), (cx - w, cy + s)]
    right = [(cx + w, cy), (cx, cy + h), (cx, cy + h + s), (cx + w, cy + s)]
    noisy(left, (121, 85, 58), 14); noisy(right, (100, 70, 48), 14); noisy(top, (98, 170, 60), 16)


def make(W, H, scale, name, tsize, blockx, textx, sub):
    rnd = random.Random(7)
    lw, lh = W // scale, H // scale
    im = sky(lw, lh, rnd)
    g = lh - lh // 6
    for x in range(lw):
        for y in range(g, lh):
            im.putpixel((x, y), (98 + rnd.randint(-12, 12), 170 + rnd.randint(-12, 12), 60 + rnd.randint(-10, 10), 255))
    s = lh // 4
    block(im, blockx // scale, lh // 2 - s // 3, s, rnd)
    big = im.resize((W, H), Image.NEAREST); d = ImageDraw.Draw(big)
    f = ImageFont.truetype(FONT, tsize)
    py = H // 2 - tsize // 2 - (8 if sub else 0)
    for o in ((2, 2), (1, 1)): d.text((textx + o[0], py + o[1]), "OptiCraft", font=f, fill=(30, 30, 30))
    d.text((textx, py), "OptiCraft", font=f, fill=(255, 255, 255))
    if sub:
        d.text((textx + 2, py + tsize + 10), sub, font=ImageFont.truetype(FONT, int(tsize * 0.36)), fill=(255, 255, 255))
    big.convert("RGB").save(os.path.join(OUT, name))


make(144, 80, 2, "ICON0.PNG", 15, 28, 52, None)
make(480, 272, 4, "PIC1.PNG", 50, 110, 200, "Heritage Edition  -  PSP")
