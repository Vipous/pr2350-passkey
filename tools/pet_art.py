# The home screen's pixel cat: palette and sprites. Edit the art here, then regenerate the C:
#   python3 tools/pet_art.py                  -> src/pet_sprites.h
#   python3 tools/pet_art.py --preview x.png  -> also a picture of every sprite (needs Pillow)
# One character per pixel, '.' is transparent; in overlays a space leaves the pixel as it was.
import os
import sys

PAL = {
    'k': 0x4A2E1A, 'o': 0xF2A03D, 'O': 0xC8731F, 'w': 0xFFF1D6, 'p': 0xFF9BB0,
    'e': 0x1A1008, 'g': 0xFFFFFF, 'b': 0x5AA9FF, 'B': 0x2E6FC0, 'r': 0xE0504A,
    'R': 0x9E2F2B, 'f': 0x8B5A2B, 'h': 0xFF4D6D, 'z': 0xA9B8FF, 'y': 0xFFE066,
    # the bomb
    'n': 0x3A3F5C, 'N': 0x1E2133, 'l': 0x7A82A8, 'a': 0xFF8C1A, 'm': 0x7A1E2C, 's': 0x7FD4FF,
    'c': 0x8E8E8E, 'C': 0x5A5A5A,
}

def mirror(rows):
    out = []
    for r in rows:
        if len(r) == 10: r = r + r[::-1]
        out.append(r)
    return out

def pad(rows, l, r):
    return ['.' * l + x + '.' * r for x in rows]

def overlay(base, patch, x0, y0):
    rows = [list(r) for r in base]
    for dy, pr in enumerate(patch):
        for dx, c in enumerate(pr):
            if c != ' ':
                rows[y0 + dy][x0 + dx] = c
    return [''.join(r) for r in rows]

# Front, sitting: 20 wide (10-character rows are mirrored), padded to 26 for the tail.
HEAD = mirror([
    "..kk......",
    "..kok.....",
    "..kopk....",
    "..koppkkkk",
    ".koooooooo",
    ".koooooOoO",
    ".koooooooo",
    ".kooogeoooooogeoook.",   # written out: both shines top-left
    ".koooeeooo",
    ".kwoooooop",
    ".kwwooooko",
    "..kwwooook",
    "...kkkooooooookkk...",
])
BODY = mirror([
    "...kooowww",
    "..kooowwww",
    "..kooowwww",
    "..koooowww",
    "..koooowww",
    "..kowwkwww",
    "..kwwwkwww",
    "...kkkkkkk",
])
SIT = pad(HEAD + BODY, 2, 4)          # 26 wide, 21 tall

TAIL_UP = [
    "   kk ",
    "  koOk",
    "  kok ",
    "  kok ",
    " kok  ",
    "kok   ",
    "ok    ",
]
TAIL_SIDE = [
    "      ",
    "      ",
    "      ",
    "   kk ",
    "  kook",
    " kokk ",
    "kok   ",
]

def sit(tail, eyes=None, mouth=None, paw=False, base=None):
    s = overlay(base or SIT, tail, 19, 12) if tail else list(base or SIT)
    if eyes == 'shut':
        s = overlay(s, ["oo      oo"], 7, 7)
        s = overlay(s, ["kk      kk"], 7, 8)
    if eyes == 'happy':
        s = overlay(s, [" kk      kk ", "kook    kook"], 6, 7)
    if mouth == 'open':
        s = overlay(s, ["kppk", " kk "], 9, 10)
    if paw:
        s = overlay(s, [" kk ", "kwwk", "kwwk", "kowk", " ko "], 5, 9)
    return s

LOAF_BODY = mirror([
    "..kooooOoo",
    ".koooooooO",
    "kooooooooo",
    "kowwkooooo",
    ".kkkkkkkkk",
])
LOAF = pad(HEAD + LOAF_BODY, 2, 4)
LOAF_TAIL = ["  kk", " kok", "kook", "kkk "]

YARN = [r.replace(" ", ".") for r in ["  kk  ", " kbBk ", "kbBbBk", "kBbBbk", " kBbk ", "  kk  "]]
BOWL = [r.replace(" ", ".") for r in [" ffffffff ", "kffffffffk", "krrrrrrrrk", " kRRRRRRk ", "  kkkkkk  "]]
HEART = [".hh.hh.", "hhhhhhh", "hhhhhhh", ".hhhhh.", "..hhh..", "...h..."]
ZED = ["zzzzz", "...z.", "..z..", ".z...", "zzzzz"]

# ---- The bomb: built from shapes rather than typed out, since it's round.
BW, BH = 30, 31
BCX, BCY, BR = 15, 16, 8.6   # body centre and radius

def canvas(w, h):
    return [['.'] * w for _ in range(h)]

def put(c, x, y, ch):
    if 0 <= y < len(c) and 0 <= x < len(c[0]):
        c[y][x] = ch

def rows_of(c):
    return [''.join(r) for r in c]

def bomb(face='smile', arms='down', legs='stand', soot=False):
    c = canvas(BW, BH)
    # Fuse: a cap on top, then a curly wick. The spark is a separate sprite at its tip.
    for (x, y) in [(15, 6), (16, 5), (17, 4), (17, 3), (18, 2), (19, 1)]:
        put(c, x, y, 'f')
    for x in range(13, 18):
        for y in (6, 7):
            put(c, x, y, 'l' if y == 6 else 'N')
    # Body: outline, fill, a highlight top-left.
    for y in range(BH):
        for x in range(BW):
            d = ((x - BCX) ** 2 + (y - BCY) ** 2) ** 0.5
            if d <= BR - 1:
                put(c, x, y, 'n')
            elif d <= BR:
                put(c, x, y, 'N')
    for (x, y) in [(10, 11), (11, 10), (12, 10), (10, 12), (11, 11)]:
        put(c, x, y, 'l')
    if soot:   # after the scare: smudged grey
        for (x, y) in [(18, 12), (19, 13), (12, 19), (17, 20), (20, 18), (13, 13)]:
            put(c, x, y, 'C')
    # Arms and gloves.
    if arms == 'down':
        pts = [(6, 17), (5, 18), (5, 19)], [(24, 17), (25, 18), (25, 19)]
        gloves = [(4, 20), (25, 20)]
    elif arms == 'swing_a':
        pts = [(6, 16), (5, 15), (4, 14)], [(24, 17), (25, 18), (25, 19)]
        gloves = [(3, 12), (25, 20)]
    elif arms == 'swing_b':
        pts = [(6, 17), (5, 18), (5, 19)], [(24, 16), (25, 15), (26, 14)]
        gloves = [(4, 20), (26, 12)]
    else:      # 'up': hands in the air
        pts = [(7, 12), (6, 11), (5, 10), (5, 9)], [(23, 12), (24, 11), (25, 10), (25, 9)]
        gloves = [(4, 7), (25, 7)]
    for side in pts:
        for (x, y) in side:
            put(c, x, y, 'l')   # limbs light, so they show on the black screen
    for (gx, gy) in gloves:
        for dx in (0, 1):
            for dy in (0, 1):
                put(c, gx + dx, gy + dy, 'w')
    # Legs and red shoes.
    if legs == 'stand':
        lx = [(12, 0), (18, 0)]
    elif legs == 'walk_a':
        lx = [(11, -1), (19, 0)]
    else:
        lx = [(11, 0), (19, -1)]
    for (x, lift) in lx:
        for y in range(25, 28 + lift):
            put(c, x, y, 'l')
        for dx in (-1, 0, 1, 2):
            put(c, x + dx, 28 + lift, 'r')
            put(c, x + dx, 29 + lift, 'R' if dx in (-1, 2) else 'r')
    # Face.
    if face in ('smile', 'blink'):
        for ex in (11, 18):
            if face == 'blink':
                for dx in range(3):
                    put(c, ex + dx, 14, 'N')
            else:
                for dx in range(3):
                    for dy in range(3):
                        put(c, ex + dx, 13 + dy, 'g')
                put(c, ex + 1, 14, 'e'); put(c, ex + 1, 15, 'e')
        put(c, 15, 16, 'l'); put(c, 16, 16, 'l')       # nose
        for (x, y) in [(12, 18), (13, 19), (14, 19), (15, 19), (16, 19), (17, 19), (18, 18)]:
            put(c, x, y, 'm')                            # smile
    else:      # 'panic': huge eyes, tiny pupils, brows up, mouth wide open
        for ex in (10, 18):
            for dx in range(4):
                for dy in range(4):
                    put(c, ex + dx, 12 + dy, 'g')
            put(c, ex + (1 if ex == 10 else 2), 13, 'e')
            for dx in range(4):
                put(c, ex + dx, 10 - (1 if dx in (1, 2) else 0), 'l')   # raised brows
        put(c, 15, 16, 'l'); put(c, 16, 16, 'l')
        for x in range(13, 19):
            for y in range(18, 21):
                put(c, x, y, 'm')
        for x in range(14, 18):
            put(c, x, 20, 'r')                           # tongue
        put(c, 22, 11, 's'); put(c, 22, 12, 's'); put(c, 23, 12, 's')   # sweat drop
    return rows_of(c)

SPARK_A = [".y.a.", "ayyya", ".yga.", "ayyya", ".a.y."]
SPARK_B = ["a...y", ".yay.", "yaga.", ".yay.", "y...a"]
SMOKE = ["..cc..", ".cCcc.", "cccCcc", ".cccc.", "..cc.."]

FRAMES = {
    'sit_a': sit(TAIL_UP),
    'sit_b': sit(TAIL_SIDE),
    'sit_blink': sit(TAIL_UP, 'shut'),
    'sit_happy': sit(TAIL_UP, 'happy'),
    'meow': sit(TAIL_UP, None, 'open'),
    'paw': sit(TAIL_UP, None, None, True),
    'groom': sit(TAIL_SIDE, 'shut', None, True),
    'eat': sit(TAIL_UP, 'shut', 'open'),
    'sleep': sit(None, 'shut', base=overlay(LOAF, LOAF_TAIL, 18, 13)),
    'yarn': YARN, 'bowl': BOWL, 'heart': HEART, 'zed': ZED,
    'bomb_stand': bomb(),
    'bomb_blink': bomb('blink'),
    'bomb_walk_a': bomb('smile', 'swing_a', 'walk_a'),
    'bomb_walk_b': bomb('smile', 'swing_b', 'walk_b'),
    'bomb_panic': bomb('panic', 'up', 'stand'),
    'bomb_panic_b': bomb('panic', 'up', 'walk_a'),
    'bomb_soot': bomb('blink', 'down', 'stand', soot=True),
    'spark_a': SPARK_A, 'spark_b': SPARK_B, 'smoke': SMOKE,
}

def check():
    for n, f in FRAMES.items():
        w = len(f[0])
        for i, r in enumerate(f):
            assert len(r) == w, (n, i, len(r), w, r)
            for c in r:
                assert c == '.' or c in PAL, (n, i, c)

def preview(path, scale=7, per_row=5):
    from PIL import Image, ImageDraw
    names = list(FRAMES)
    cw = max(len(FRAMES[n][0]) for n in names) * scale + 20
    ch = max(len(FRAMES[n]) for n in names) * scale + 30
    rows = (len(names) + per_row - 1) // per_row
    img = Image.new('RGB', (cw * per_row, ch * rows), (0, 0, 0))
    d = ImageDraw.Draw(img)
    for i, n in enumerate(names):
        ox, oy = (i % per_row) * cw + 10, (i // per_row) * ch + 10
        for y, r in enumerate(FRAMES[n]):
            for x, c in enumerate(r):
                if c != '.':
                    v = PAL[c]
                    d.rectangle([ox + x * scale, oy + y * scale, ox + x * scale + scale - 1, oy + y * scale + scale - 1],
                                fill=(v >> 16, v >> 8 & 255, v & 255))
        d.text((ox, oy + ch - 26), n, fill=(150, 150, 150))
    img.save(path)

def emit_c(path):
    names = list(FRAMES)
    out = ["// Generated by tools/pet_art.py from the sprites drawn there; don't edit by hand.",
           "#pragma once", "", "#include <stdint.h>", "",
           "typedef struct {", "    uint8_t w, h;", "    const char *const *rows;", "} pet_sprite_t;", "",
           "static uint32_t pet_color(char c) {", "    switch (c) {"]
    for c, v in PAL.items():
        out.append(f"    case '{c}': return 0x{v:06X};")
    out += ["    default: return 0;", "    }", "}", ""]
    out.append("enum {")
    for n in names:
        out.append(f"    PET_{n.upper()},")
    out += ["    PET_SPRITE_COUNT", "};", ""]
    for n in names:
        rows = ",\n".join(f'    "{r}"' for r in FRAMES[n])
        out.append(f"static const char *const pet_rows_{n}[] = {{\n{rows},\n}};")
    out.append("")
    out.append("static const pet_sprite_t pet_sprites[PET_SPRITE_COUNT] = {")
    for n in names:
        out.append(f"    [PET_{n.upper()}] = {{{len(FRAMES[n][0])}, {len(FRAMES[n])}, pet_rows_{n}}},")
    out += ["};", ""]
    with open(path, 'w') as f:
        f.write("\n".join(out))

check()
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
emit_c(os.path.join(root, 'src', 'pet_sprites.h'))
if '--preview' in sys.argv:
    preview(sys.argv[sys.argv.index('--preview') + 1])
