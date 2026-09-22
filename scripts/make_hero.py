# Builds the Nexus page hero image (1920x1080) from the mod's own prefab renders. Usage: python make_hero.py [out.png]
import sys, os, random
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont, ImageFilter
import numpy as np
ROOT = Path(__file__).resolve().parent.parent
THUMBS = ROOT / "asi" / "cdmodkit" / "data" / "thumbs"
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "release" / "nexus_hero.png"
W, H = 1920, 1080

def pick(n):
    files = sorted(os.listdir(THUMBS))
    random.Random(7).shuffle(files)
    good = []
    for f in files:
        im = Image.open(THUMBS / f).convert("RGBA"); a = np.array(im)[:, :, 3] > 0
        cov = a.mean()
        ys, xs = np.where(a)
        if len(xs) == 0: continue
        w = xs.max() - xs.min() + 1; h = ys.max() - ys.min() + 1
        if 0.14 < cov < 0.5 and 0.6 < w / h < 1.8: good.append(f)
        if len(good) >= n: break
    return [Image.open(THUMBS / f).convert("RGBA") for f in good]

def tint(im, rgb):
    a = np.array(im).astype(np.float32)
    lum = a[:, :, :3].mean(axis=2, keepdims=True) / 214.0
    col = np.array(rgb, dtype=np.float32)[None, None, :]
    a[:, :, :3] = np.clip(lum * col, 0, 255)
    return Image.fromarray(a.astype(np.uint8), "RGBA")

img = Image.new("RGBA", (W, H), (0, 0, 0, 255))
d = ImageDraw.Draw(img)
# background: dark warm gradient with a subtle vignette
for y in range(H):
    t = y / H
    d.line([(0, y), (W, y)], fill=(int(28 + 18 * t), int(20 + 10 * t), int(18 + 8 * t), 255))
vign = Image.new("L", (W, H), 0); vd = ImageDraw.Draw(vign)
vd.ellipse([-W * 0.2, -H * 0.5, W * 1.2, H * 1.5], fill=255)
vign = vign.filter(ImageFilter.GaussianBlur(220))
img = Image.composite(img, Image.new("RGBA", (W, H), (10, 8, 8, 255)), vign)

# prefab renders: a loose 4x2 grid on the right two thirds, warm tinted, with soft shadows
thumbs = pick(8)
cell_w, cell_h = 250, 250
grid_x0, grid_y0 = 800, 150
for i, t in enumerate(thumbs):
    cx = grid_x0 + (i % 4) * (cell_w + 30) + cell_w // 2
    cy = grid_y0 + (i // 4) * (cell_h + 110) + cell_h // 2 + (30 if i % 2 else 0)
    t = tint(t, (236, 214, 178)).resize((cell_w, cell_h), Image.LANCZOS)
    sh = Image.new("RGBA", (cell_w + 60, cell_h + 60), (0, 0, 0, 0))
    sh.paste(Image.new("RGBA", t.size, (0, 0, 0, 140)), (30, 42), t)
    sh = sh.filter(ImageFilter.GaussianBlur(14))
    img.alpha_composite(sh, (cx - cell_w // 2 - 30, cy - cell_h // 2 - 30))
    img.alpha_composite(t, (cx - cell_w // 2, cy - cell_h // 2))

# accent bar + title block on the left
d = ImageDraw.Draw(img)
d.rectangle([90, 210, 104, 620], fill=(214, 96, 58, 255))
def font(name, size):
    for p in (f"C:/Windows/Fonts/{name}",):
        if os.path.exists(p): return ImageFont.truetype(p, size)
    return ImageFont.load_default()
title = font("segoeuib.ttf", 100); sub = font("segoeui.ttf", 44); small = font("segoeuisl.ttf", 34)
d.text((130, 190), "World Builder", font=title, fill=(245, 236, 224, 255))
d.text((134, 340), "In-game prefab editor", font=sub, fill=(232, 200, 170, 255))
d.text((134, 395), "& modding SDK for Crimson Desert", font=sub, fill=(232, 200, 170, 255))
lines = ["48,000+ prefabs, browse & spawn live", "place, rotate, scale with hotkeys", "save builds as projects", "C API for other mods"]
for i, l in enumerate(lines):
    y = 500 + i * 46
    d.ellipse([136, y + 12, 148, y + 24], fill=(214, 96, 58, 255))
    d.text((165, y), l, font=small, fill=(220, 210, 198, 255))
d.text((130, 1000), "Ultimate ASI Loader plugin  |  no game files modified  |  previews rendered from your own install", font=font("segoeuisl.ttf", 26), fill=(150, 138, 128, 255))
img.convert("RGB").save(OUT, quality=95)
print("->", OUT, f"{len(thumbs)} renders")
