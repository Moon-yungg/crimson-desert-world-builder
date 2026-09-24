# Nexus title image for Fly Mode: the in-game aerial screenshot without the HUD, a dark gradient on the left, title, key and credits.
#   python scripts\make_flymode_banner.py <screenshot.jpg> <out.png>
import sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter

src, out = sys.argv[1], sys.argv[2]
W, H = 1920, 1080
img = Image.open(src).convert("RGB")
sw, sh = img.size
# crop away the HUD: resource counters top right, quest text and minimap on the left; keep 16:9
x0, y0 = int(sw * 0.211), int(sh * 0.097)
cw = sw - x0; ch = int(cw * 9 / 16)
if y0 + ch > sh: ch = sh - y0; cw = int(ch * 16 / 9)
img = img.crop((x0, y0, x0 + cw, y0 + ch)).resize((W, H), Image.LANCZOS)

# left-to-right dark gradient for the text, a light vignette at the bottom
from PIL import ImageChops
side = Image.new("L", (W, H), 0); d = ImageDraw.Draw(side)
for x in range(W):
    a = max(0.0, 1.0 - x / (W * 0.72)); d.line([(x, 0), (x, H)], fill=int(238 * a ** 1.15))
bottom = Image.new("L", (W, H), 0); d = ImageDraw.Draw(bottom)
for y in range(H):
    a = max(0.0, (y - H * 0.72) / (H * 0.28)); d.line([(0, y), (W, y)], fill=int(120 * a))
shade = ImageChops.lighter(side, bottom).filter(ImageFilter.GaussianBlur(8))
img = Image.composite(Image.new("RGB", (W, H), (8, 10, 14)), img, shade)

F = r"C:\Windows\Fonts\\"
title = ImageFont.truetype(F + "georgiab.ttf", 150)
sub = ImageFont.truetype(F + "segoeuil.ttf", 46)
small = ImageFont.truetype(F + "segoeui.ttf", 34)
keyf = ImageFont.truetype(F + "segoeuib.ttf", 58)
credit = ImageFont.truetype(F + "segoeuisl.ttf", 36)
d = ImageDraw.Draw(img)
gold, white, grey = (232, 196, 120), (245, 242, 235), (200, 204, 210)
X = 110

def text(xy, s, font, fill, shadow=True, spacing=0):
    if shadow: d.text((xy[0] + 3, xy[1] + 4), s, font=font, fill=(0, 0, 0))
    d.text(xy, s, font=font, fill=fill)

text((X, 250), "FLY MODE", title, white)
d.line([(X + 4, 438), (X + 520, 438)], fill=gold, width=4)
text((X, 468), "A free camera for Crimson Desert", sub, grey)

# key cap
kx, ky = X, 590
d.rounded_rectangle([kx + 4, ky + 6, kx + 134, ky + 116], 16, fill=(0, 0, 0))
d.rounded_rectangle([kx, ky, kx + 130, ky + 110], 16, fill=(38, 42, 50), outline=gold, width=4)
tw = d.textlength("F7", font=keyf); d.text((kx + (130 - tw) / 2, ky + 16), "F7", font=keyf, fill=white)
text((kx + 165, ky + 12), "on / off", sub, white)
text((kx + 165, ky + 66), "WASD  \u00b7  Mouse  \u00b7  Space / Ctrl  \u00b7  Shift", small, grey)

text((X, H - 120), "\u00a9 dofo7777 and Zappenduster", credit, gold)
img.save(out, quality=92)
print("written", out, img.size)
