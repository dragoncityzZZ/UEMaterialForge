"""Generate the MaterialForge application icon and PNG copy."""
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
SIZE = 1024

img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
draw = ImageDraw.Draw(img)

margin = 48
radius = 190
draw.rounded_rectangle(
    (margin, margin, SIZE - margin, SIZE - margin),
    radius=radius,
    fill=(13, 25, 30, 255),
    outline=(38, 206, 188, 255),
    width=22,
)
draw.rounded_rectangle(
    (margin + 34, margin + 34, SIZE - margin - 34, SIZE - margin - 34),
    radius=radius - 30,
    outline=(38, 206, 188, 45),
    width=6,
)

teal = (64, 231, 210, 255)
amber = (255, 184, 78, 255)
muted = (70, 132, 141, 255)
draw.line(
    [(270, 700), (270, 330), (520, 500), (760, 310)],
    fill=teal,
    width=24,
    joint="curve",
)
draw.line([(270, 700), (520, 500)], fill=muted, width=14)
draw.line([(760, 310), (520, 500)], fill=amber, width=20)


def node(cx: int, cy: int, r: int, color: tuple[int, int, int, int]) -> None:
    draw.ellipse(
        (cx - r, cy - r, cx + r, cy + r),
        fill=(13, 25, 30, 255),
        outline=color,
        width=18,
    )
    draw.ellipse(
        (cx - r // 3, cy - r // 3, cx + r // 3, cy + r // 3),
        fill=color,
    )


node(270, 700, 72, teal)
node(270, 330, 72, teal)
node(520, 500, 72, teal)
node(760, 310, 72, amber)

draw.polygon(
    [(512, 345), (650, 512), (512, 679), (374, 512)],
    fill=(30, 84, 91, 255),
    outline=(128, 255, 238, 255),
)

(ROOT / "resources").mkdir(exist_ok=True)
(ROOT / "assets").mkdir(exist_ok=True)
img.save(
    ROOT / "resources" / "MaterialForge.ico",
    sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)],
)
img.resize((256, 256), Image.LANCZOS).save(ROOT / "assets" / "MaterialForge.png")
print("Wrote resources/MaterialForge.ico and assets/MaterialForge.png")
