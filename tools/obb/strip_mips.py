#!/usr/bin/env python3
"""strip_mips.py -- a Switch-sized copy of YOUR Space Hulk OBB (prototype).

The port loads textures from their second mip (sh_quality.c,
masterTextureLimit = 1: the 1 GB of a 32-bit Switch program cannot hold the
2048x2048 ETC2 textures), but Unity still reads and inflates the top mip of
every texture from the OBB, then throws it away. This writes a copy of the
OBB whose mipmapped textures start at their second mip already: the same
picture with masterTextureLimit = 0, ~75% less texture data to read.

    python3 tools/obb/strip_mips.py main.7.com.hoplite.spacehulk.obb out.obb [--only level6]

Textures without mipmaps (most of the UI's) are left alone, as
masterTextureLimit leaves them -- and so are the GUI atlases that HAVE
mipmaps ("Menu GUI", 1024x2048, 12 mips): NGUI places its sprites in pixels
of texture.width / height, which masterTextureLimit leaves at the full size
but a stripped texture halves -- the menus' selection frames came out as
solid green blocks (hardware 2026-10-06). Every other entry is copied as it is. The output's entries are
deflated like the input's. An entry "assets/switch-mips-stripped" marks the
result, for the port. Needs UnityPy.
"""
import argparse
import collections
import io
import re
import sys
import zipfile

import UnityPy

SPLIT = 1024 * 1024  # Unity's .splitN chunk size on Android

# bytes of a w x h level, by Unity TextureFormat
BLOCK = {34: 8, 45: 8, 46: 8, 47: 16, 10: 8, 12: 16}  # ETC_RGB4, ETC2_RGB, ETC2_RGBA1, ETC2_RGBA8, DXT1, DXT5
PIXEL = {1: 1, 3: 3, 4: 4, 5: 4, 7: 2, 13: 2, 14: 4}  # Alpha8, RGB24, RGBA32, ARGB32, RGB565, RGBA4444, BGRA32


def level_bytes(fmt, w, h):
    if fmt in BLOCK:
        return ((w + 3) // 4) * ((h + 3) // 4) * BLOCK[fmt]
    if fmt in PIXEL:
        return w * h * PIXEL[fmt]
    return None


# NGUI atlases and other GUI textures: sprites addressed in pixels (see above)
KEEP = re.compile(r"GUI")


def strip(tree, stats):
    w, h, mips, fmt = tree["m_Width"], tree["m_Height"], tree["m_MipCount"], tree["m_TextureFormat"]
    if KEEP.search(tree.get("m_Name", "")):
        stats["GUI textures kept"] += 1
        return False
    if mips <= 1 or w < 8 or h < 8 or tree.get("m_ImageCount", 1) != 1:
        return False
    top = level_bytes(fmt, w, h)
    data = tree["image data"]
    if top is None or not isinstance(data, (bytes, bytearray)) or len(data) <= top:
        stats["skipped (format/data)"] += 1
        return False
    tree["image data"] = bytes(data[top:])
    tree["m_Width"], tree["m_Height"], tree["m_MipCount"] = w // 2, h // 2, mips - 1
    tree["m_CompleteImageSize"] = len(tree["image data"])
    stats["textures stripped"] += 1
    stats["bytes saved"] += top
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("obb")
    ap.add_argument("out")
    ap.add_argument("--only", help="regex of the serialized files to process (test)")
    a = ap.parse_args()
    zin = zipfile.ZipFile(a.obb)
    groups = collections.defaultdict(list)
    for i in zin.infolist():
        m = re.match(r"(assets/bin/Data/(?:level\d+|sharedassets\d+\.assets|resources\.assets|[0-9a-f]{32}))(?:\.split(\d+))?$", i.filename)
        if m:
            groups[m.group(1)].append((int(m.group(2)) if m.group(2) else -1, i.filename))
    done = set()
    stats = collections.Counter()
    zout = zipfile.ZipFile(a.out, "w", zipfile.ZIP_DEFLATED, compresslevel=6)
    for base, parts in sorted(groups.items()):
        names = [f for _, f in sorted(parts)]
        if a.only and not re.search(a.only, base):
            continue
        data = b"".join(zin.read(f) for f in names)
        env = UnityPy.load(io.BytesIO(data))
        changed = 0
        for o in env.objects:
            if o.type.name != "Texture2D":
                continue
            tree = o.read_typetree()
            if strip(tree, stats):
                o.save_typetree(tree)
                changed += 1
        if not changed:
            continue
        sf = list(env.files.values())[0]
        new = sf.save()
        split = parts[0][0] >= 0
        if split:
            for k in range(0, len(new), SPLIT):
                zout.writestr(f"{base}.split{k // SPLIT}", new[k:k + SPLIT])
        else:
            zout.writestr(base, new)
        done.update(names)
        stats["files rewritten"] += 1
        print(f"{base}: {changed} textures, {len(data) >> 20} -> {len(new) >> 20} MB", flush=True)
    for i in zin.infolist():
        if i.filename not in done:
            zout.writestr(i, zin.read(i.filename), compress_type=i.compress_type)
    zout.writestr("assets/switch-mips-stripped", b"1\n")
    zout.close()
    for k, v in stats.items():
        print(f"{k}: {v >> 20 if k == 'bytes saved' else v}{' MB' if k == 'bytes saved' else ''}")


if __name__ == "__main__":
    main()
