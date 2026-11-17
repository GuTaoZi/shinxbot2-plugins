"""Builds the celesteguessr plugin's data from ScreenshotTool scans and Celeste's own map files.

History of user requests (2026-11-17, GuTao): a CelesteGuessr-like game (celesteguessr.com: one screenshot from
anywhere in the game, guess where it is, up to 5000 points) as a bot plugin. QQ has no clicking on a map, so players
pick the chapter first, get that chapter's map, then answer a room number read off it (user: "first guess the chapter,
then provide a map, then number"). CelesteGuessr's own map is tile silhouettes, not screenshots, so players must
recognise a room's shape, not match pictures; here it is drawn like Celeste's F6 debug map, which players know
(solid white, air dark), with the numbers in small corner badges ("nobody really remember the room number, most of
us just remember the shape of landscape"). Screenshots come from d-002's ScreenshotTool (MIT) chapter
scans, with the player hidden and no skin or HUD mods; the scan is started without keyboard focus by a CelesteTAS
line: `Invoke, SilentScanner.StartScanChapter, Player`.

Rooms are numbered along the chapter: a breadth-first tree over rooms that touch (within 24 px), the route = the tree
path from the start room to the end room, and a depth-first walk numbers each room's side branches right after it.

Output, per side, in OUT/<key>/ (key: 0, 1A..7C, E, 8A..8C, 9):
  side.json      key, names, rooms [{num, name, x, y, w, h, part, local}] (num along the whole side; part/local =
                 section and number within it, as players answer "7A-3 5"), shots [{file, room (num), x, y}]
                 (camera centre, world px),
                 maps [{file, scale (px per world px), ox, oy (world px at the picture's 0,0), rooms [num]}]
                 (overview = maps[0], for plugin builds before sections)
  overview.png   the map (silhouettes + room numbers), or map_1.png ... one per checkpoint section of a big side
  shots/*.jpg    question pictures, 960x540

Run on the machine with the game and the scans (Windows here), then copy OUT to the bot's
resource/celesteguessr/ directory:
  python cg_build_data.py --celeste "C:/Program Files (x86)/Steam/steamapps/common/Celeste" --out cg_data [--only 1A]
"""
import argparse
import glob
import json
import math
import os
import re
import struct
from collections import deque
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# map file stem -> (number shown to players, side letters by file suffix)
CHAPTERS = {
    "0-Intro": ("0", 0), "1-ForsakenCity": ("1", 1), "2-OldSite": ("2", 2), "3-CelestialResort": ("3", 3),
    "4-GoldenRidge": ("4", 4), "5-MirrorTemple": ("5", 5), "6-Reflection": ("6", 6), "7-Summit": ("7", 7),
    "8-Epilogue": ("E", 8), "9-Core": ("8", 9), "LostLevels": ("9", 10),
}
SINGLE_SIDE = {"0", "E", "9"}   # chapters with only an A side: key without the letter
# Rooms nobody plays (CelesteGuessr's rooms are hand-checked too): 7A's credits scenes, Farewell's cutscene rooms.
SKIP_PREFIX = ("credits",)
SKIP_ROOMS = {"end-cinematic", "end-granny", "intro-00-past"}
# Where the chapter starts when it isn't the map's first room (checked against AreaData.Load in Celeste.dll,
# 2026-11-17, user: "I think 9-8 you mark is actually 9-9, and 9-9 is for golden berry-specific single room"): Farewell's
# first checkpoint (一个人 Singular) is a-00, so the intro rooms before it are the Start section; 6A's bonfire room "start"
# lies apart from the rest and is its Start section ("check 6A's starting plot part").
START_ROOM = {"LostLevels": "intro-01-future"}
FIRST_SECTION = {"6-Reflection": ["start"]}


def read_map(path):
    """Celeste's .bin map (Monocle BinaryPacker) as {name, attrs, children}."""
    b, i = Path(path).read_bytes(), [0]

    def u8():
        i[0] += 1
        return b[i[0] - 1]

    def fmt(f, n):
        i[0] += n
        return struct.unpack_from(f, b, i[0] - n)[0]

    def s():
        n = shift = 0
        while True:
            c = u8()
            n |= (c & 0x7F) << shift
            shift += 7
            if c < 0x80:
                break
        i[0] += n
        return b[i[0] - n:i[0]].decode("utf-8")

    assert s() == "CELESTE MAP"
    s()
    table = [s() for _ in range(fmt("<h", 2))]

    def value(t):
        if t == 0:
            return bool(u8())
        if t == 1:
            return u8()
        if t == 2:
            return fmt("<h", 2)
        if t == 3:
            return fmt("<i", 4)
        if t == 4:
            return fmt("<f", 4)
        if t == 5:
            return table[fmt("<h", 2)]
        if t == 6:
            return s()
        if t == 7:
            n = fmt("<h", 2)
            raw = b[i[0]:i[0] + n]
            i[0] += n
            return "".join(chr(raw[k + 1]) * raw[k] for k in range(0, n, 2))
        raise ValueError(t)

    def element():
        name = table[fmt("<h", 2)]
        attrs = {}
        for _ in range(u8()):
            k = table[fmt("<h", 2)]
            attrs[k] = value(u8())
        return {"name": name, "attrs": attrs, "children": [element() for _ in range(fmt("<h", 2))]}

    return element()


def area_names(celeste):
    """AREA_n names from the game's own English and Simplified Chinese dialog files."""
    out = {}
    for lang in ("English", "Simplified Chinese"):
        names = {}
        for line in (Path(celeste) / "Content/Dialog" / f"{lang}.txt").read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line.startswith("AREA_") and "=" in line:
                k, v = line.split("=", 1)
                if k.strip()[5:].isdigit():
                    names[int(k.strip()[5:])] = v.strip()
        out[lang] = names
    return out


def touch(a, b, gap=24):
    ax2, ay2, bx2, by2 = a["x"] + a["w"], a["y"] + a["h"], b["x"] + b["w"], b["y"] + b["h"]
    h_adj = (0 <= b["x"] - ax2 <= gap or 0 <= a["x"] - bx2 <= gap) and min(ay2, by2) - max(a["y"], b["y"]) > 0
    v_adj = (0 <= b["y"] - ay2 <= gap or 0 <= a["y"] - by2 <= gap) and min(ax2, bx2) - max(a["x"], b["x"]) > 0
    return h_adj or v_adj


def number_rooms(rooms, start, end=None):
    key = lambda n: (rooms[n]["x"], -rooms[n]["y"])
    nbrs = {n: [m for m in rooms if m != n and touch(rooms[n], rooms[m])] for n in rooms}
    parent, dist, q = {start: None}, {start: 0}, deque([start])
    while q:
        r = q.popleft()
        for m in sorted(nbrs[r], key=key):
            if m not in dist:
                dist[m], parent[m] = dist[r] + 1, r
                q.append(m)
    end = end if end in dist else max(dist, key=dist.get)
    route, r = set(), end
    while r is not None:
        route.add(r)
        r = parent[r]
    children = {n: [] for n in rooms}
    for m, p in parent.items():
        if p is not None:
            children[p].append(m)
    order = []
    stack = [start]
    while stack:   # depth-first, side branches before the route
        n = stack.pop()
        order.append(n)
        stack.extend(sorted(children[n], key=lambda c: (c in route, *key(c)), reverse=True))
    order += sorted((n for n in rooms if n not in dist), key=key)
    for i, n in enumerate(order, 1):
        rooms[n]["num"] = i
    return order, set(dist)


def grid(text, w, h):
    rows = (text or "").split("\n")
    return [[(rows[y][x] if y < len(rows) and x < len(rows[y]) else "0") != "0" for x in range(w)] for y in range(h)]


def font(size):
    for f in ("C:/Windows/Fonts/arialbd.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"):
        if os.path.exists(f):
            return ImageFont.truetype(f, size)
    return ImageFont.load_default()


MAX_PX = 3000   # a map picture's longer side; bigger chapters are split into checkpoint sections
PER_ROOM = 8    # screenshots kept per room (7A and Farewell scan 1400-2000; the zip must reach Volcano)


def tile_scale(rooms, names):
    L = min(rooms[n]["x"] for n in names); T = min(rooms[n]["y"] for n in names)
    R = max(rooms[n]["x"] + rooms[n]["w"] for n in names); B = max(rooms[n]["y"] + rooms[n]["h"] for n in names)
    return max(1, min(4, int(MAX_PX / max((R - L) // 8, (B - T) // 8, 1))))


def sections(rooms, order, checkpoints, reachable):
    """The map in pictures (2026-11-17: 7A at one pixel per tile is 3600 px square, Farewell 18400 px wide - too
    small to read in QQ): a section from each checkpoint room along the route - every side with checkpoints, not only
    big ones (user: "if player directly guess *cg 5A 4, then the whole 5A will be calculated as same chapter ...
    do not allow non-chapter whole room guess"); rooms the route never reaches (Farewell's golden-berry room) are
    grouped by touch.
    When every checkpoint is an "x-00" room of its own letter (5A: b-00 c-00 d-00 e-00), the vanilla names say the
    section: 5A's d and e areas sit beside its c area, so walking by touch mixed them (2026-11-17)."""
    letter = lambda n: n[0] if re.match(r"[a-z]-", n) else None
    cps = sorted(checkpoints)
    if cps and all(re.fullmatch(r"[a-z]-00", c) for c in cps) and len({c[0] for c in cps}) == len(cps):
        letters = sorted({c[0] for c in cps} | ({letter(order[0])} if letter(order[0]) else set()))
        parts = [[n for n in order if letter(n) == l] for l in letters]
        parts = [p for p in parts if p]
        rest = [n for n in order if letter(n) not in letters]
        if letter(order[0]) is None:   # 8A starts in "00", "01"...: the rooms without a letter are its Start section
            parts.insert(0, [n for n in rest if n in reachable])
            rest = [n for n in rest if n not in reachable]
    else:
        parts, cur = [], []
        for n in order:
            if n not in reachable:
                continue
            if n in checkpoints and cur:
                parts.append(cur)
                cur = []
            cur.append(n)
        if cur:
            parts.append(cur)
        rest = [n for n in order if n not in reachable]
    while rest:   # connected groups of the unreached rooms
        group, todo = [], [rest.pop(0)]
        while todo:
            n = todo.pop()
            group.append(n)
            for m in [m for m in rest if touch(rooms[n], rooms[m])]:
                rest.remove(m)
                todo.append(m)
        parts.append(sorted(group, key=lambda n: rooms[n]["num"]))
    return parts


def draw_overview(rooms, order, path, label=""):
    """The chapter as Celeste's F6 debug map draws it (2026-11-17, user: players "just remember the shape of
    landscape", shown the debug map): solid tiles white, air dark, each room's number in a small badge in its top-left
    corner so it doesn't cover the shape. Players pick the chapter first, get this map, then answer the number."""
    L = min(rooms[n]["x"] for n in order); T = min(rooms[n]["y"] for n in order)
    R = max(rooms[n]["x"] + rooms[n]["w"] for n in order); B = max(rooms[n]["y"] + rooms[n]["h"] for n in order)
    tw, th = (R - L) // 8, (B - T) // 8
    s = tile_scale(rooms, order)          # px per tile
    pad = 24
    img = Image.new("RGB", (tw * s + 2 * pad, th * s + 2 * pad), (14, 14, 18))
    px = img.load()
    for n in order:
        r = rooms[n]
        w, h = r["w"] // 8, r["h"] // 8
        fg = grid(r["solids"], w, h)
        x0, y0 = pad + (r["x"] - L) // 8 * s, pad + (r["y"] - T) // 8 * s
        for y in range(h):
            for x in range(w):
                c = (236, 236, 236) if fg[y][x] else (36, 40, 48)
                for dy in range(s):
                    for dx in range(s):
                        px[x0 + x * s + dx, y0 + y * s + dy] = c
    d = ImageDraw.Draw(img, "RGBA")
    for n in order:
        r = rooms[n]
        x0, y0 = pad + (r["x"] - L) / 8 * s, pad + (r["y"] - T) / 8 * s
        d.rectangle((x0, y0, x0 + r["w"] / 8 * s - 1, y0 + r["h"] / 8 * s - 1), outline=(80, 130, 170), width=2)
    fs = max(11, min(20, int(s * 5)))
    f = font(fs)
    for n in order:
        r = rooms[n]
        x0, y0 = pad + (r["x"] - L) / 8 * s, pad + (r["y"] - T) / 8 * s
        t = str(r.get("local", r["num"]))
        bw, bh = d.textlength(t, font=f) + fs * 0.6, fs * 1.25
        d.rounded_rectangle((x0 + 3, y0 + 3, x0 + 3 + bw, y0 + 3 + bh), radius=bh / 3, fill=(251, 114, 153, 235))
        d.text((x0 + 3 + bw / 2, y0 + 3 + bh / 2), t, font=f, fill=(255, 255, 255), anchor="mm")
    if label:   # which section of the chapter this picture is
        lf = font(22)
        d.rounded_rectangle((6, 4, 16 + d.textlength(label, font=lf), 34), radius=8, fill=(0, 0, 0, 170))
        d.text((11, 19), label, font=lf, fill=(255, 214, 0), anchor="lm")
    img.save(path)
    return {"file": Path(path).name, "scale": s / 8, "ox": L - pad * 8 / s, "oy": T - pad * 8 / s,
            "rooms": [rooms[n]["num"] for n in order]}


def nonsense(img):
    """Mostly black or flat pictures (a fade, an empty sky) make no question."""
    g = img.convert("L").resize((160, 90))
    hist = g.histogram()
    dark = sum(hist[:24]) / (160 * 90)
    return dark > 0.9


def build_side(celeste, exports, stem, side, out, names):
    num, area = CHAPTERS[stem]
    key = num if num in SINGLE_SIDE else num + side
    suffix = {"A": "", "B": "H", "C": "X"}[side]
    bin_path = Path(celeste) / "Content/Maps" / f"{stem.replace('-', suffix + '-', 1) if suffix else stem}.bin"
    if stem == "LostLevels":
        bin_path = Path(celeste) / "Content/Maps/LostLevels.bin"
    scan = Path(exports) / "Celeste" / stem / side
    m = read_map(bin_path)
    levels = next(c for c in m["children"] if c["name"] == "levels")["children"]
    scanned = {p.name for p in scan.iterdir() if p.is_dir()} if scan.exists() else set()
    rooms, checkpoints = {}, set()
    for lv in levels:
        a = lv["attrs"]
        name = a["name"][4:] if a["name"].startswith("lvl_") else a["name"]
        if scanned and name not in scanned or name.startswith(SKIP_PREFIX) or name in SKIP_ROOMS:
            continue
        ch = {c["name"]: c for c in lv["children"]}
        if any(e["name"] == "checkpoint" for e in ch.get("entities", {"children": []})["children"]):
            checkpoints.add(name)
        rooms[name] = {"name": name, "x": a["x"], "y": a["y"], "w": a["width"], "h": a["height"],
                       "solids": ch.get("solids", {"attrs": {}})["attrs"].get("innerText", ""),
                       "bg": ch.get("bg", {"attrs": {}})["attrs"].get("innerText", "")}
    start = next(iter(rooms))   # the map's first room is where the chapter starts
    for lv in levels:
        n = lv["attrs"]["name"]
        n = n[4:] if n.startswith("lvl_") else n
        if n in rooms:
            start = n
            break
    start = START_ROOM.get(stem, start) if START_ROOM.get(stem) in rooms else start
    # a "start" room that touches nothing (5B, 6A) is the side's own Start section; 2A's is part of its first section
    pre = [n for n in FIRST_SECTION.get(stem, ["start"]) if n in rooms
           and not any(touch(rooms[n], rooms[m]) for m in rooms if m != n)]
    main = {n: r for n, r in rooms.items() if n not in pre}
    order, reachable = number_rooms(main, start, "end" if "end" in main else None)
    order = pre + order   # global numbers: the separate Start section first
    for i, n in enumerate(order, 1):
        rooms[n]["num"] = i
    od = Path(out) / key
    (od / "shots").mkdir(parents=True, exist_ok=True)
    # (2026-11-17, user: "maybe use things like 7A-3 5? and send forward folded messages after chosen a chapter, with
    # each subchapter a image") rooms are numbered within their section: a guess is "7A-3 5"; one-section sides
    # keep "1A 19" (section 1).
    parts = ([pre] if pre else []) + sections(rooms, [n for n in order if n not in pre], checkpoints, reachable)
    maps = []
    for i, part in enumerate(parts, 1):
        for j, n in enumerate(part, 1):
            rooms[n]["part"], rooms[n]["local"] = i, j
        label = f"{key}-{i} · {len(part)} 个房间" if len(parts) > 1 else ""
        maps.append(draw_overview(rooms, part, od / (f"map_{i}.png" if len(parts) > 1 else "overview.png"), label))
    overview = maps[0]
    shots, skipped = [], 0
    for n in order:
        files = sorted(glob.glob(str(scan / n / "*.png")))
        if len(files) > PER_ROOM:   # evenly spread over the room (CelesteGuessr: 6725 shots for 818 rooms)
            files = [files[round(i * (len(files) - 1) / (PER_ROOM - 1))] for i in range(PER_ROOM)]
        for f in files:
            parts = Path(f).stem.split(",")
            dx, dy = int(parts[1][3:]), int(parts[2][3:])
            img = Image.open(f).convert("RGB")
            if nonsense(img):
                skipped += 1
                continue
            name = f"{rooms[n]['num']:03d}_{n}_{dx}_{dy}.jpg"
            img.resize((960, 540), Image.LANCZOS).save(od / "shots" / name, quality=82)
            shots.append({"file": "shots/" + name, "room": rooms[n]["num"], "x": rooms[n]["x"] + dx, "y": rooms[n]["y"] + dy})
    side_name = "" if num in SINGLE_SIDE else f" {side}面"
    data = {
        "key": key, "sid": f"Celeste/{stem}", "side": side,
        "name_en": names["English"].get(area, stem) + ("" if num in SINGLE_SIDE else f" {side}-Side"),
        "name_zh": names["Simplified Chinese"].get(area, stem) + side_name,
        "overview": overview,
        "maps": maps,   # one picture per section; overview = the first (older plugin builds read only that)
        "rooms": [{k: rooms[n][k] for k in ("num", "name", "x", "y", "w", "h", "part", "local")} for n in order],
        "shots": shots,
    }
    (od / "side.json").write_text(json.dumps(data, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"{key}: {len(rooms)} rooms, {len(shots)} shots ({skipped} skipped), {len(maps)} map(s) "
          + " ".join(f"{Image.open(od / m['file']).size}" for m in maps))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--celeste", required=True)
    ap.add_argument("--exports", help="ScreenshotTool's export folder (default <celeste>/ScreenshotTool_Exports)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", nargs="*", help="keys to build, e.g. 1A 2B 9")
    a = ap.parse_args()
    exports = a.exports or str(Path(a.celeste) / "ScreenshotTool_Exports")
    names = area_names(a.celeste)
    for stem, (num, _) in CHAPTERS.items():
        for side in "ABC":
            key = num if num in SINGLE_SIDE else num + side
            if num in SINGLE_SIDE and side != "A":
                continue
            if a.only and key not in a.only:
                continue
            if not (Path(exports) / "Celeste" / stem / side).exists():
                continue
            build_side(a.celeste, exports, stem, side, a.out, names)


if __name__ == "__main__":
    main()
