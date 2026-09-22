# Mod page thumbnail from a screenshot: python make_thumbnail.py <screenshot> [out.jpg]
import sys, os
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageEnhance
ROOT = Path(__file__).resolve().parent.parent
src = Path(sys.argv[1]); OUT = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "release" / "WorldBuilder_thumbnail.jpg"
W, H = 1920, 1080

im = Image.open(src).convert("RGB")
# cover-crop to 16:9
sw, sh = im.size
if sw / sh > W / H: nh = sh; nw = int(sh * W / H)
else: nw = sw; nh = int(sw * H / W)
im = im.crop(((sw - nw) // 2, (sh - nh) // 2, (sw - nw) // 2 + nw, (sh - nh) // 2 + nh)).resize((W, H), Image.LANCZOS)
im = ImageEnhance.Contrast(im).enhance(1.08); im = ImageEnhance.Color(im).enhance(1.1)
im = im.convert("RGBA")

# legibility: dark gradient from the bottom and a soft one from the left
grad = Image.new("L", (1, H))
for y in range(H):
    t = max(0.0, (y - H * 0.42) / (H * 0.58))
    grad.putpixel((0, y), int(255 * (t ** 1.4) * 0.82))
shade = Image.new("RGBA", (W, H), (8, 6, 6, 255)); shade.putalpha(grad.resize((W, H)))
im.alpha_composite(shade)
lgrad = Image.new("L", (W, 1))
for x in range(W):
    t = max(0.0, 1 - x / (W * 0.62))
    lgrad.putpixel((x, 0), int(255 * (t ** 1.6) * 0.55))
shade = Image.new("RGBA", (W, H), (8, 6, 6, 255)); shade.putalpha(lgrad.resize((W, H)))
im.alpha_composite(shade)

def font(name, size):
    p = f"C:/Windows/Fonts/{name}"
    return ImageFont.truetype(p, size) if os.path.exists(p) else ImageFont.load_default()
title = font("segoeuib.ttf", 150); sub = font("segoeuisl.ttf", 52); small = font("segoeuisl.ttf", 34)

# text layer with a blurred shadow underneath
txt = Image.new("RGBA", (W, H), (0, 0, 0, 0)); d = ImageDraw.Draw(txt)
x0, y0 = 110, 660
d.rectangle([x0 - 34, y0 + 28, x0 - 20, y0 + 300], fill=(224, 96, 52, 255))
d.text((x0, y0), "World Builder", font=title, fill=(250, 244, 234, 255))
d.text((x0 + 6, y0 + 186), "In-game prefab editor & modding SDK for Crimson Desert", font=sub, fill=(236, 214, 186, 255))
d.text((x0 + 8, y0 + 262), "48,000+ prefabs   |   place, rotate, scale with hotkeys   |   save builds as projects   |   C API", font=small, fill=(210, 200, 190, 255))
shadow = txt.split()[3].filter(ImageFilter.GaussianBlur(10))
sh = Image.new("RGBA", (W, H), (0, 0, 0, 0)); sh.putalpha(shadow.point(lambda a: int(a * 0.85)))
im.alpha_composite(sh, (4, 6)); im.alpha_composite(txt)

im.convert("RGB").save(OUT, quality=93)
print("->", OUT)
