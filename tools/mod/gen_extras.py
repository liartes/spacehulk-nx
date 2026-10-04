#!/usr/bin/env python3
"""gen_extras.py -- mod/src/ExtrasData.cs, the figurines the SEA game has the
models for but never lists.

Two kinds, all built from meshes already in the SEA game's own bundles:

* the seven the world-wide edition (Disney Crossy Road 3.x) released and the
  SEA edition never got: Golden Camel and Dragon Genie (Aladdin), Elf
  Pleakley, Santa Jumba, Vampire Stitch and Witch Lilo (Lilo & Stitch), The
  Ocean (Moana). Their recipes (animation frames, scales, hop sounds, config
  line) come from that edition's Character objects;
* the unreleased ones, whose models and names are in both editions' files
  but which no Character ever used: Classic Mickey, the Fantasia Broom,
  Safari Mickey, Max, Oswald, Ortensia (Mickey & Friends), the Scuba Diver
  (Finding Dory) and Human Cadenza (Beauty and the Beast). Their recipes are
  written here, in the style of their theme's figurines.

References only (mesh GUIDs, names, scales): no game data is copied. Inputs:
the SEA game's assets/AssetBundles/guids-to-asset-mapping.txt, and the
world-wide edition's Characters as JSON (tools/mod/ducktales/dumpchars.py):

  python3 tools/mod/gen_extras.py <sea guids-to-asset-mapping.txt> <dcr chars.json>

MIT.
"""
import json
import os
import sys

# the world-wide edition's figurines: id -> (SEA figurine to copy the in-game body from, English name)
FROM_DCR = [
    ("3060", "2100", "Golden Camel"),
    ("3061", "2100", "Dragon Genie"),
    ("2827", "2801", "Witch Lilo"),
    ("2825", "2801", "Vampire Stitch"),
    ("2822", "2801", "Elf Pleakley"),
    ("2823", "2801", "Santa Jumba"),
    ("2320", "2301", None),  # "The Ocean": the SEA tables have the name
]

# The world-wide config lines, made obtainable: their event and hidden-unlock
# flags point at events this edition does not run, so each is in the prize
# machine instead, weighted by rarity like the SEA game's own.
RARE, EPIC, SPECIAL, COMMON = ("333", "333", "250"), ("166", "166", "142"), ("42", "42", "35"), ("1000", "1000", "1000")


FLAGS = ["secret", "featured", "rare", "epic", "legendary", "enchanted", "diamond", "hiddenUnlock", "credits",
         "dailyStamp", "toyLinked", "eventPrize", "hidden", "inPrizeMachine", "forPurchase", "bundle"]


def line(i, name, sort, uni, rarity, weights):
    # columns: defineCharacter id name sort universe unlockedByDefault, the 16
    # FLAGS, iap, collection sets, prize machine / pixel machine / maxed weights
    cols = ["NO"] * len(FLAGS)
    if rarity:
        cols[FLAGS.index(rarity)] = "YES"
    cols[FLAGS.index("inPrizeMachine")] = "YES"
    return "\t".join(["defineCharacter", i, name, str(sort), str(uni), "NO"] + cols + ["none", "none"] + list(weights))


CONFIG = {
    "3060": line("3060", "Golden Camel", 31, 17, "rare", RARE),
    "3061": line("3061", "Dragon Genie", 32, 17, "enchanted", SPECIAL),
    "2827": line("2827", "Witch Lilo", 328, 23, "rare", RARE),
    "2825": line("2825", "Vampire Stitch", 335, 23, "enchanted", SPECIAL),
    "2822": line("2822", "Elf Pleakley", 341, 23, "rare", RARE),
    "2823": line("2823", "Santa Jumba", 345, 23, "enchanted", SPECIAL),
    "2320": line("2320", "The Ocean", 402, 19, "diamond", SPECIAL),
    # the unreleased ones: next to their theme's relatives
    "801": line("801", "Classic Mickey", 117, 2, None, COMMON),
    "861": line("861", "Broom", 117, 2, "rare", RARE),
    "803": line("803", "Safari Mickey", 120, 2, "rare", RARE),
    "871": line("871", "Max", 139, 2, None, COMMON),
    "860": line("860", "Oswald", 159, 2, "epic", EPIC),
    "869": line("869", "Ortensia", 159, 2, "rare", RARE),
    "1372": line("1372", "Scuba Diver", 214, 14, "rare", RARE),
    "2734": line("2734", "Human Cadenza", 87, 22, "rare", RARE),
}

VC = "VertexColourCharacters"


def a(name, nxt, speed, meshes, mat=VC):
    return {"name": name, "next": nxt, "speed": speed, "frames": [(m, mat) for m in meshes]}


def still(pre, mesh):
    return a(pre + "idle-1", pre + "idle-1", 0.0, [mesh])


def hops(pre, meshes):
    return [a(pre + "jump-%d" % (k + 1), pre + "idle-1", 6.0, [m]) for k, m in enumerate(meshes)]


def unreleased():
    out = []

    def add(i, template, anim, scale, zoom, idle, jump=(), select=(), behaviour=0, name=None, generic_hop=None):
        out.append({"id": i, "template": template, "anim": anim, "name": name, "scale": scale, "zoom": zoom,
                    "idleBehaviour": behaviour, "hop": None, "genericHop": generic_hop,
                    "select": list(select), "idle": list(idle), "jump": list(jump)})

    p = "char-disney_friends_ClassicMickey-"
    add("801", "937", "disney_friends_ClassicMickey", 0.8, 0.7, [still(p, "disney_classicmickey_idle_1")])
    p = "char-disney_friends_FantasiaBroom-"
    b0, b1, b2 = "disney_fantasiabroom_idle_1", "disney_fantasiabroom_jump_1", "disney_fantasiabroom_jump_2"
    add("861", "937", "disney_friends_FantasiaBroom", 0.8, 0.7,
        [still(p, b0), a(p + "idle-2", p + "idle-1", 4.0, [b0, b1, b0, b2, b0, b1, b0, b2])],
        hops(p, [b1, b2]), select=[a(p + "select-1", p + "select-1", 4.0, [b0, b1, b0, b2])], behaviour=2)
    p = "char-disney_friends_SafariMickey-"
    s0, s1 = "disney_safarimickey_idle_1", "disney_safarimickey_arm_stretch"
    # his one other pose (the arm stretched) is his hop, as the Sorcerer's wand is his
    add("803", "937", "disney_friends_SafariMickey", 0.8, 0.7,
        [still(p, s0), a(p + "idle-2", p + "idle-1", 3.0, [s0, s1, s1, s1, s0])], hops(p, [s1]),
        select=[a(p + "select-1", p + "select-1", 1.5, [s0, s1])], behaviour=2)
    add("871", "937", "disney_friends_Max", 0.8, 0.7, [still("char-disney_friends_Max-", "disney_max_idle_1")])
    add("860", "937", "disney_friends_Oswald", 0.8, 0.7, [still("char-disney_friends_Oswald-", "disney_oswald_idle_1")])
    add("869", "937", "disney_friends_Ortensia", 0.8, 0.7, [still("char-disney_friends_Ortensia-", "disney_ortensia_idle_1")])
    p = "char-FindingDory_ScubaDiver-"
    add("1372", "1376", "FindingDory_ScubaDiver", 1.0, 0.35, [still(p, "findingdory_scubadiver")],
        hops(p, ["findingdory_scubadiver_jump_1", "findingdory_scubadiver_jump_2"]))
    p = "char-BeautyAndTheBeast_HumanCadenza-"
    add("2734", "2733", "BeautyAndTheBeast_HumanCadenza", 0.8, 0.3, [still(p, "beautyandthebeast_humancadenza_idle_1")],
        hops(p, ["beautyandthebeast_humancadenza_hop_1", "beautyandthebeast_humancadenza_hop_2"]), name="Human Cadenza")
    return out


def from_dcr(chars):
    by_id = {c["id"]: c for c in chars.values()}
    out = []
    for i, template, name in FROM_DCR:
        c = by_id[i]

        def conv(lst):
            return [{"name": x[0], "next": x[1], "speed": x[2],
                     "frames": [(f[0], f[2].split(":", 1)[1].split("#")[0]) for f in x[3]]} for x in lst]
        out.append({"id": i, "template": template, "anim": c["anim"], "name": name,
                    "scale": c["selectScreenScale"], "zoom": c["selectedZoomPercentage"],
                    "idleBehaviour": c["idleAnimationBehaviour"],
                    "hop": c["hopAudioKey"] or None, "genericHop": c["genericHopsKey"] or None,
                    "select": conv(c["selectAnimations"]), "idle": conv(c["idleAnimations"]), "jump": conv(c["jumpAnimations"])})
    return out


def cs(s):
    return "null" if s is None else '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    guid = {}
    for l in open(sys.argv[1], encoding="utf-8"):
        p = l.rstrip("\r\n").split("\t")
        if len(p) == 3 and p[1].endswith("-characters"):
            guid[p[2].replace("_optimised.asset", "").replace(".asset", "")] = p[0]
    chars = from_dcr(json.load(open(sys.argv[2]))) + unreleased()

    def anims(lst):
        if not lst:
            return "new A[0]"
        r = []
        for x in lst:
            fr = ", ".join("new F(%s, %s, true)" % (cs(guid[m]), cs(mat)) for m, mat in x["frames"])
            r.append("new A(%s, %s, %sf, new F[] { %s })" % (cs(x["name"]), cs(x["next"]), repr(float(x["speed"])), fr))
        return "new A[] {\n                    " + ",\n                    ".join(r) + " }"

    L = ["// ExtrasData.cs -- GENERATED by tools/mod/gen_extras.py; do not edit.",
         "// Figurines built from models already in the SEA game's own bundles: mesh GUIDs,",
         "// names, scales and config lines (references only; no game data is in here).",
         "using A = DcrMod.DuckTalesData.A;",
         "using F = DcrMod.DuckTalesData.F;",
         "",
         "namespace DcrMod",
         "{",
         "    public static class ExtrasData",
         "    {",
         "        public class X",
         "        {",
         "            public string Id, Template, Anim, Name, Hop, GenericHop, Config;",
         "            public float Scale, Zoom;",
         "            public int IdleBehaviour;",
         "            public A[] Select, Idle, Jump;",
         "        }",
         "        public static readonly X[] Chars = {"]
    for c in chars:
        L.append("            new X {")
        L.append("                Id = %s, Template = %s, Anim = %s, Name = %s," % (cs(c["id"]), cs(c["template"]), cs(c["anim"]), cs(c["name"])))
        L.append("                Hop = %s, GenericHop = %s, Scale = %rf, Zoom = %rf, IdleBehaviour = %d," % (
            cs(c["hop"]), cs(c["genericHop"]), float(c["scale"]), float(c["zoom"]), int(c["idleBehaviour"])))
        L.append("                Config = %s," % cs(CONFIG[c["id"]]))
        L.append("                Select = %s," % anims(c["select"]))
        L.append("                Idle = %s," % anims(c["idle"]))
        L.append("                Jump = %s," % anims(c["jump"]))
        L.append("            },")
    L += ["        };", "    }", "}"]
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "..", "..", "mod", "src", "ExtrasData.cs")
    open(out, "w").write("\n".join(L) + "\n")
    print("gen_extras.py: %d figurines -> %s" % (len(chars), os.path.normpath(out)))


if __name__ == "__main__":
    main()
