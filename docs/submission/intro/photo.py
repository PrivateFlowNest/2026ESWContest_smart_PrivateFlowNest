# 작품 사진 for the 작품소개서: the real model (a still from the 2026-10-27 footage, assets/model-photo-wide.jpg)
# with numbered markers on the devices and a legend band. Positions were checked against the footage: the upper
# LED lit on "거실 불 켜 줘", the lower one on "에어컨 켜 줘", the red frame opened on "창문 열어 줘".
#   python -I photo.py   -> out/model-annotated.png
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
FONT = HERE.parents[2] / "team" / "homemind-core-ui-2026-10-20" / "homemind_flutter_test" / "assets" / "fonts" / "NotoSansKR-VariableFont_wght.ttf"
ORANGE = (232, 116, 59)

# (number, point on the device, marker centre, legend text)
MARKS = [
    (1, (820, 280), (700, 215), "창문 · 열기/닫기 모터(ESP32)"),
    (2, (950, 370), (1030, 330), "거실 조명 LED"),
    (3, (950, 510), (1030, 555), "에어컨 표시등 LED"),
    (4, (1104, 300), (1200, 250), "화장실 환기팬(ESP8266)"),
]


def font(size, weight):
    f = ImageFont.truetype(str(FONT), size)
    f.set_variation_by_axes([weight])
    return f


def main():
    im = Image.open(HERE / "assets" / "model-photo-wide.jpg").convert("RGBA")
    layer = Image.new("RGBA", im.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    num = font(30, 800)
    for n, (px, py), (cx, cy), _ in MARKS:
        d.line((px, py, cx, cy), fill=(255, 255, 255, 235), width=4)
        d.ellipse((px - 7, py - 7, px + 7, py + 7), fill=(255, 255, 255, 255), outline=ORANGE + (255,), width=3)
        d.ellipse((cx - 25, cy - 25, cx + 25, cy + 25), fill=ORANGE + (255,), outline=(255, 255, 255, 255), width=3)
        label = str(n)
        l, t, r, b = d.textbbox((0, 0), label, font=num)
        d.text((cx - (r - l) / 2 - l, cy - (b - t) / 2 - t), label, font=num, fill="white")
    # legend band along the bottom (over the model's front panel)
    w, h = im.size
    band = 64
    d.rectangle((0, h - band, w, h), fill=(8, 26, 33, 215))
    f = font(27, 600)
    x = 24
    for n, _, _, text in MARKS:
        d.ellipse((x, h - band / 2 - 15, x + 30, h - band / 2 + 15), fill=ORANGE + (255,))
        l, t, r, b = d.textbbox((0, 0), str(n), font=font(20, 800))
        d.text((x + 15 - (r - l) / 2 - l, h - band / 2 - (b - t) / 2 - t), str(n), font=font(20, 800), fill="white")
        d.text((x + 38, h - band / 2 - 17), text, font=f, fill=(234, 246, 248))
        x += 38 + d.textlength(text, font=f) + 34
    out = HERE / "out" / "model-annotated.png"
    out.parent.mkdir(exist_ok=True)
    Image.alpha_composite(im, layer).convert("RGB").save(out)
    print(out, im.size, "legend width", int(x))


if __name__ == "__main__":
    main()
