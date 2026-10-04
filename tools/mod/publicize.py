#!/usr/bin/env python3
"""publicize.py -- reference assemblies for compiling the port's C# (mod/).

The port's own C# (mod/src) is compiled against the game's managed DLLs, read
from YOUR copy of the game (an apktool folder or the APK itself). They are
copied to mod/refs/ -- which is never shipped and never committed -- with
Assembly-CSharp and Assembly-CSharp-firstpass "publicized": every type,
method and field marked public, so the C# can name the game's private
members. Only the compiler sees these copies; at run time the mod binds to
the game's real assemblies by name (see mod/README.md for how private members
are reached there).

Fields named like an event or a property of the same type stay as they are
(a field-like event's backing field has the event's name: making it public
would make the name ambiguous to the compiler).

  python3 tools/mod/publicize.py <apktool dir | game.apk> [out dir]

Needs dnfile (pip install dnfile). MIT.
"""
import io
import os
import sys
import zipfile

import dnfile

PUBLICIZE = ("Assembly-CSharp.dll", "Assembly-CSharp-firstpass.dll")
MANAGED = "assets/bin/Data/Managed/"


def publicize(data: bytes) -> bytes:
    pe = dnfile.dnPE(data=data)
    md = pe.net.mdtables
    out = bytearray(data)

    def patch16(table, i, off, fn):
        at = table.file_offset + i * table.row_size + off
        v = int.from_bytes(out[at:at + 2], "little")
        out[at:at + 2] = fn(v).to_bytes(2, "little")

    def patch32(table, i, off, fn):
        at = table.file_offset + i * table.row_size + off
        v = int.from_bytes(out[at:at + 4], "little")
        out[at:at + 4] = fn(v).to_bytes(4, "little")

    # types: top level -> Public (1), nested -> NestedPublic (2)
    for i, _ in enumerate(md.TypeDef):
        patch32(md.TypeDef, i, 0,
                lambda f: (f & ~7) | (2 if (f & 7) >= 2 else 1))

    # event / property names per type (their rows are listed per owner type)
    member_names = {}
    for tab, lst, key in ((md.EventMap, "EventList", "Name"),
                          (md.PropertyMap, "PropertyList", "Name")):
        if tab is None:
            continue
        for row in tab:
            owner = row.Parent.row_index
            for m in getattr(row, lst):
                member_names.setdefault(owner, set()).add(str(m.row.Name))

    # fields and methods, by owner type (TypeDef.FieldList / MethodList runs)
    for ti, t in enumerate(md.TypeDef):
        names = member_names.get(ti + 1, set())
        for f in t.FieldList:
            if str(f.row.Name) in names:
                continue
            patch16(md.Field, f.row_index - 1, 0, lambda v: (v & ~7) | 6)
        for m in t.MethodList:
            patch16(md.MethodDef, m.row_index - 1, 6, lambda v: (v & ~7) | 6)
    return bytes(out)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    here = os.path.dirname(os.path.abspath(__file__))
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "..", "..", "mod", "refs")
    os.makedirs(dst, exist_ok=True)
    files = {}
    if os.path.isdir(src):
        d = os.path.join(src, MANAGED)
        for n in os.listdir(d):
            if n.endswith(".dll"):
                files[n] = open(os.path.join(d, n), "rb").read()
    else:
        with zipfile.ZipFile(src) as z:
            for n in z.namelist():
                if n.startswith(MANAGED) and n.endswith(".dll") and "/" not in n[len(MANAGED):]:
                    files[n[len(MANAGED):]] = z.read(n)
    if "Assembly-CSharp.dll" not in files:
        sys.exit("publicize.py: no %sAssembly-CSharp.dll in %s" % (MANAGED, src))
    for n, data in sorted(files.items()):
        if n in PUBLICIZE:
            data = publicize(data)
        with open(os.path.join(dst, n), "wb") as f:
            f.write(data)
    print("publicize.py: %d reference assemblies in %s (%s publicized)"
          % (len(files), os.path.normpath(dst), ", ".join(PUBLICIZE)))


if __name__ == "__main__":
    main()
