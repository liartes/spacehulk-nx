#!/usr/bin/env python3
"""make_icon.py -- launcher/icon.jpg from YOUR copy of the game's APK.

The launcher's icon is the game's own (res/drawable-xxxhdpi-v4/app_icon.png),
so it is not in the repository: this makes it, 256x256 JPEG on black.

    python3 tools/make_icon.py <Space Hulk APK>

Needs Pillow.
"""
import io
import os
import sys
import zipfile

from PIL import Image

ICON = "res/drawable-xxxhdpi-v4/app_icon.png"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    with zipfile.ZipFile(sys.argv[1]) as z:
        im = Image.open(io.BytesIO(z.read(ICON))).convert("RGBA")
    bg = Image.new("RGB", im.size, (0, 0, 0))
    bg.paste(im, mask=im.split()[3])
    out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "launcher", "icon.jpg")
    bg.resize((256, 256), Image.LANCZOS).save(out, quality=92)
    print("wrote", out)


if __name__ == "__main__":
    main()
