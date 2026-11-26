"""Generate the image-decoder test corpus (and a sample sheet) from scratch.

Everything the image probe needs comes out of this script, so the corpus is
reproducible on any machine with Pillow installed and does not depend on any
binary that happens to be lying around.

    python tests/gen_image_corpus.py build/imgtest

It writes one file per format/variant plus a `<name>.raw` holding the RGBA
bytes a correct decoder must produce (PIL's own decode of the encoded file —
for lossy JPEG that is emphatically *not* the pre-encoding source).
"""

import os
import sys

from PIL import Image

W, H = 24, 16


def base_rgb():
    """A small image with a diagonal gradient plus a few hard-edged blocks, so
    that both smooth gradients (which expose IDCT / dithering errors) and sharp
    edges (which expose upsampling and filter errors) are present."""
    image = Image.new("RGB", (W, H))
    pixels = image.load()
    for y in range(H):
        for x in range(W):
            r = (x * 255) // (W - 1)
            g = (y * 255) // (H - 1)
            b = ((x + y) * 255) // (W + H - 2)
            # a saturated block and a dark block to pin the extremes
            if 4 <= x < 8 and 2 <= y < 5:
                r, g, b = 255, 40, 10
            elif 14 <= x < 19 and 9 <= y < 13:
                r, g, b = 8, 12, 90
            pixels[x, y] = (r, g, b)
    return image


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else "build/imgtest"
    os.makedirs(directory, exist_ok=True)
    rgb = base_rgb()

    def save(name, image, **kwargs):
        path = os.path.join(directory, name)
        image.save(path, **kwargs)
        decoded = Image.open(path).convert("RGBA")
        with open(path + ".raw", "wb") as handle:
            handle.write(decoded.tobytes())
        print("%-16s %s" % (name, decoded.size))

    # --- PNG: every colour type Pillow can write, plus interlace -------------
    save("rgba32.png", rgb.convert("RGBA"))
    save("rgb24.png", rgb)
    save("gray.png", rgb.convert("L"))
    save("bw.png", rgb.convert("1"))
    save("pal.png", rgb.convert("P", palette=Image.ADAPTIVE, colors=64))
    save("rgb16.png", rgb.convert("RGB").quantize(colors=32).convert("RGB"))
    save("interlaced.png", rgb, interlace=True)

    # --- JPEG: baseline, greyscale, and progressive -------------------------
    save("rgb.jpg", rgb, quality=85)
    save("gray.jpg", rgb.convert("L"), quality=85)
    save("prog.jpg", rgb, quality=75, progressive=True)

    # --- GIF: single frame and a two-frame animation ------------------------
    save("rgb.gif", rgb.convert("P", palette=Image.ADAPTIVE, colors=256))
    frames = [rgb.convert("P", palette=Image.ADAPTIVE, colors=64)]
    shifted = base_rgb().rotate(180)
    frames.append(shifted.convert("P", palette=Image.ADAPTIVE, colors=64))
    frames[0].save(os.path.join(directory, "anim.gif"), save_all=True, append_images=frames[1:],
                   duration=100, loop=0)
    path = os.path.join(directory, "anim.gif")
    with open(path + ".raw", "wb") as handle:
        handle.write(Image.open(path).convert("RGBA").tobytes())
    print("%-16s %s" % ("anim.gif", Image.open(path).size))

    # --- BMP: 24bpp and an 8bpp paletted image ------------------------------
    save("rgb24.bmp", rgb)
    pal = rgb.convert("P", palette=Image.ADAPTIVE, colors=16)
    save("pal8.bmp", pal)
    save("gray.bmp", rgb.convert("L"))


if __name__ == "__main__":
    main()
