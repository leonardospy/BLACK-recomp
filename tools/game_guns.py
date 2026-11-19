# The game's own first person guns for VR: reads assets/levels/*/fpguns/bg1_*.db (the player's copy of the game) and
# writes vr_assets/g_<name>.bvr, the format src/vr/models.cpp reads. Plain Python 3 with numpy (Blender's will do):
#   python tools/game_guns.py <assets dir> <vr_assets dir>
# The files are made from the player's own game data, so they stay out of the repository.
#
# A .db holds the gun and the arms holding it, skinned, in one vertex buffer (36 bytes a vertex: f32 position,
# u8 weights[4], s16 bone palette offsets[4], NORMPACKED3 normal, f32 uv), with a record per mesh:
#   f32 bbox max[3], 0, f32 bbox min[3], 0, u32 primitive (6: strip), u32 index count, u32 index offset (from the
#   record), 16 bit indices into the buffer that starts after the last record.
# The diffuse texture's Xbox format word sits at 0x18C and its DXT blocks at 0x200 (alpha: the gloss).
# Each bone moves one part: the part a vertex follows is the bone of its largest weight.
# The game's space: metres, muzzle +z, top +y, origin about the trigger.
#
# .bvr version 2 (BVR1 with these additions), little endian:
#   "BVR2" u32 vertices u32 indices u32 joints u32 parts
#   vertices: f32 pos[3] f32 normal[3] f32 colour[3] u8 joints[4] f32 weights[4] u32 part f32 uv[2]   (68 bytes)
#   indices, parts, joints as BVR1
#   u32 texture width, u32 height, RGBA8 rows top down; f32 slide travel (metres the slide moves back on a shot);
#   f32 fore[3] (where the left hand holds it), u32 flags (1 both hands, 2 a pump); f32 right[16] (the right hand's
#   place on this gun, column major: moves the hands' right one, made on the pistol), f32 fore_rot[16] (the left
#   hand's turn on the fore end)
import glob
import os
import struct
import sys

import numpy as np

# per gun file: the parts by bone (palette offset): magazine, slide (or bolt, charging handle, pump), and how far the
# slide goes back on a shot; grip: the palm's centre in the game's space when the default finds the wrong place;
# hide: bones not drawn; shell: a shotgun's shell, at its loading port (part "shell", the one loaded by hand); two: held with both hands, the left on the fore end;
# pump: both hands, the left on the pump, which racks a round after each shot
GUNS = {
    "pst": dict(magazine=114, slide=[117], travel=0.030),   # FTP_ pistol
    "gk1": dict(magazine=117, slide=[120], travel=0.025),   # FTgk1_ Glock
    "mgn": dict(magazine=None, slide=[], travel=0.0),       # FTmgn_ revolver
    "shg": dict(magazine=None, slide=[111], travel=0.08, shell=114, pump=True),    # FTSG_ shotgun
    "rm1": dict(magazine=None, slide=[117], travel=0.08, shell=120, pump=True),    # FTrm1_ pump shotgun
    "snr": dict(magazine=81, slide=[84], travel=0.07, two=True, mirror_uv=True),       # FTSR_ sniper (bolt action)
    "smg": dict(magazine=114, slide=[117], travel=0.040),   # FTSMG_
    "sm3": dict(magazine=111, slide=[117], travel=0.030),   # FTsm3_
    "p90": dict(magazine=78, slide=[81], travel=0.030, two=True),     # FTP90_
    "mp1": dict(magazine=78, slide=[84], travel=0.030, two=True),     # FTmp1_
    "asr": dict(magazine=78, slide=[81], travel=0.040, two=True),     # FTAR_
    "m16": dict(magazine=78, slide=[84], travel=0.050, two=True),     # FTM16_
    "ak1": dict(magazine=81, slide=[84], travel=0.060, two=True),     # FTAK_
}


# the palm in the game's gun space (the pistol's grip, where the game's right hand holds every gun)
PALM = np.array([0.0, -0.012, -0.068])


def meshes(db):
    out = []
    for p in range(0, len(db) - 44, 4):
        if struct.unpack_from("<I", db, p + 12)[0] or struct.unpack_from("<I", db, p + 28)[0]:
            continue
        mx = struct.unpack_from("<3f", db, p)
        mn = struct.unpack_from("<3f", db, p + 16)
        prim, cnt, off = struct.unpack_from("<III", db, p + 32)
        if prim not in (5, 6) or not 3 <= cnt < 200000 or p + off + cnt * 2 > len(db):
            continue
        if not all(np.isfinite(mx + mn)) or not all(a > b for a, b in zip(mx, mn)) or max(map(abs, mx + mn)) > 10:
            continue
        out.append(dict(rec=p, max=mx, min=mn, prim=prim, count=cnt, ioff=p + off))
    return out


def dxt(data, w, h, fmt):
    bw, bh = max(w // 4, 1), max(h // 4, 1)
    bs = 8 if fmt == 0x0C else 16
    blk = np.frombuffer(data[:bw * bh * bs], np.uint8).reshape(bh * bw, bs)
    cb = blk[:, -8:]
    c0 = cb[:, 0].astype(np.uint32) | cb[:, 1].astype(np.uint32) << 8
    c1 = cb[:, 2].astype(np.uint32) | cb[:, 3].astype(np.uint32) << 8

    def rgb(c):
        return np.stack([(c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31],
                        1).astype(np.float32)

    a, b = rgb(c0), rgb(c1)
    four = (c0 > c1) | (fmt != 0x0C)
    pal = np.stack([a, b, np.where(four[:, None], (2 * a + b) / 3, (a + b) / 2),
                    np.where(four[:, None], (a + 2 * b) / 3, 0)], 1)
    bits = cb[:, 4:8].copy().view("<u4")[:, 0]
    sel = np.stack([(bits >> (2 * i)) & 3 for i in range(16)], 1)
    col = pal[np.arange(len(blk))[:, None], sel]
    alpha = np.full((len(blk), 16), 255.0)
    if fmt == 0x0E:
        ab = blk[:, :8].copy().view("<u8")[:, 0]
        alpha = np.stack([((ab >> np.uint64(4 * i)) & np.uint64(15)).astype(np.float32) * 17 for i in range(16)], 1)
    elif fmt == 0x0F:
        a0, a1 = blk[:, 0].astype(np.float32), blk[:, 1].astype(np.float32)
        ab = np.zeros(len(blk), np.uint64)
        for i in range(6):
            ab |= blk[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
        s = np.stack([((ab >> np.uint64(3 * i)) & np.uint64(7)).astype(np.int64) for i in range(16)], 1)
        eight = a0 > a1
        tab = np.zeros((len(blk), 8))
        tab[:, 0], tab[:, 1] = a0, a1
        for i in range(1, 7):
            tab[:, i + 1] = np.where(eight, ((7 - i) * a0 + i * a1) / 7, ((5 - i) * a0 + i * a1) / 5 if i < 5 else 0)
        tab[:, 6] = np.where(eight, tab[:, 6], 0)
        tab[:, 7] = np.where(eight, tab[:, 7], 255)
        alpha = tab[np.arange(len(blk))[:, None], s]
    rgba = np.concatenate([col, alpha[:, :, None]], 2)
    rgba = rgba.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return rgba.clip(0, 255).astype(np.uint8)


def strip(ix):
    tris = []
    for i in range(len(ix) - 2):
        a, b, c = ix[i], ix[i + 1], ix[i + 2]
        if a != b and b != c and a != c:
            tris.append((a, b, c) if i % 2 == 0 else (b, a, c))
    return np.array(tris, np.int64)


def read(path):
    db = open(path, "rb").read()
    ms = meshes(db)
    vb = max(m["rec"] for m in ms) + 0x30
    # the arms: both of them, 0.386 m across (their reach back varies a little from gun to gun)
    arms = [m for m in ms if abs(m["max"][0] - 0.193) < 0.003 and abs(m["min"][0] + 0.193) < 0.003]
    gun = max((m for m in ms if m not in arms), key=lambda m: m["count"])
    ix = np.frombuffer(db, np.uint16, gun["count"], gun["ioff"]).astype(np.int64)
    rec = np.frombuffer(db, np.uint8, (ix.max() + 1) * 36, vb).reshape(-1, 36)
    n = len(rec)
    pos = rec[:, 0:12].copy().view(np.float32).reshape(n, 3)
    w = rec[:, 12:16].astype(np.float32)
    bone = rec[:, 16:24].copy().view(np.int16).reshape(n, 4)
    pk = rec[:, 24:28].copy().view(np.uint32)[:, 0]
    nrm = np.stack([((pk << 21).astype(np.int32) >> 21) / 1023, ((pk << 10).astype(np.int32) >> 21) / 1023,
                    (pk.astype(np.int32) >> 22) / 511], 1)
    nrm = np.maximum(nrm, -1)
    nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-6)
    uv = rec[:, 28:36].copy().view(np.float32).reshape(n, 2)
    group = bone[np.arange(n), w.argmax(1)]
    fmtword = struct.unpack_from("<I", db, 0x18C)[0]
    tw, th, tf = 1 << (fmtword >> 20 & 15), 1 << (fmtword >> 24 & 15), fmtword >> 8 & 0xFF
    tex = dxt(db[0x200:], tw, th, tf)
    return pos, nrm, uv, group, strip(ix), tex


def grip_of(pos, group, tris, body_groups):
    # the pistol grip: the body below the trigger and behind it; the palm sits in its upper part
    used = np.unique(tris)
    p = pos[used][np.isin(group[used], body_groups)]
    sel = p[(p[:, 2] > -0.12) & (p[:, 2] < -0.015) & (p[:, 1] < -0.02) & (p[:, 1] > -0.07)]
    if len(sel) < 10:
        return np.array([0.0, -0.012, -0.05])
    return np.array([sel[:, 0].mean(), -0.012, sel[:, 2].mean()])


def mirror_bare_side(pos, nrm, uv, group, tris, tex):
    """The game shows a gun from its left only, and some leave the right bare: faces there mapped on a flat patch of
    the texture. Those take the left side's mapping, mirrored: each corner the uv of the left side's vertex nearest its
    mirror image (new vertices, so the faces round them keep theirs)"""
    h, w = tex.shape[:2]
    lum = tex[:, :, :3].astype(np.float32).mean(2)
    a, b, c = pos[tris[:, 0]], pos[tris[:, 1]], pos[tris[:, 2]]
    fn = np.cross(b - a, c - a)
    fn /= np.maximum(np.linalg.norm(fn, axis=1, keepdims=True), 1e-9)
    # which way is out: the face's normal against its vertices'
    fn *= np.sign((fn * nrm[tris].sum(1)).sum(1, keepdims=True) + 1e-9)
    left = np.unique(tris[fn[:, 0] > 0.6])
    out_pos, out_nrm, out_uv, out_group, tris = list(pos), list(nrm), list(uv), list(group), tris.copy()
    bare = 0
    for t in np.nonzero(fn[:, 0] < -0.6)[0]:
        tri = uv[tris[t]]
        ws = np.random.default_rng(int(t)).dirichlet((1, 1, 1), 24)
        su = ws @ tri
        px = lum[((su[:, 1] % 1.0) * h).astype(int).clip(0, h - 1), ((su[:, 0] % 1.0) * w).astype(int).clip(0, w - 1)]
        if px.std() > 4.0:
            continue
        bare += 1
        for k in range(3):
            v = tris[t, k]
            m = pos[v] * np.array([-1, 1, 1])
            near = left[np.argmin(np.linalg.norm(pos[left] - m, axis=1))]
            out_pos.append(pos[v]); out_nrm.append(nrm[v]); out_uv.append(uv[near]); out_group.append(group[v])
            tris[t, k] = len(out_pos) - 1
    # faces mapped on a single point (all their corners at one uv: flat grey, seen through the right side's opening)
    # take the outer left plate's mapping, the nearest of its vertices at the same height and length
    plate = left[pos[left, 0] > 0.01]
    flat = 0
    for t in range(len(tris)):
        corners = np.array(out_uv)[tris[t]]
        if np.abs(corners - corners[0]).max() > 1e-6:
            continue
        flat += 1
        for k in range(3):
            v = tris[t, k]
            near = plate[np.argmin(np.linalg.norm(pos[plate, 1:] - out_pos[v][1:], axis=1))]
            out_pos.append(out_pos[v]); out_nrm.append(out_nrm[v]); out_uv.append(uv[near]); out_group.append(out_group[v])
            tris[t, k] = len(out_pos) - 1
    print(f"  mirrored the left's mapping onto {bare} bare faces of the right side, mapped {flat} unmapped faces")
    return np.array(out_pos), np.array(out_nrm), np.array(out_uv), np.array(out_group), tris


def convert(name, cfg, path, out, pistol=None):
    pos, nrm, uv, group, tris, tex = read(path)
    # the gun's own vertices only (the buffer holds the arms' too)
    used, tris = np.unique(tris, return_inverse=True)
    tris = tris.reshape(-1, 3)
    pos, nrm, uv, group = pos[used], nrm[used], uv[used], group[used]
    if cfg.get("mirror_uv"):
        pos, nrm, uv, group, tris = mirror_bare_side(pos, nrm, uv, group, tris, tex)
    if cfg.get("hide"):
        tris = tris[~np.isin(group[tris], cfg["hide"]).any(1)]
    special = ([cfg["magazine"]] if cfg["magazine"] is not None else []) + cfg["slide"] + ([cfg["shell"]] if cfg.get("shell") is not None else [])
    body = [g for g in np.unique(group).tolist() if g not in special]
    # the game holds every gun the same way in the hand (its origin about the trigger): the pistol's palm for all, so
    # the trigger finger meets each trigger as in the game
    grip = np.array(cfg["grip"]) if "grip" in cfg else PALM
    # the game's space (x left, y up, muzzle +z) to the VR one (x right, barrel -z): half a turn about y; origin at the palm
    P = (pos - grip) * np.array([-1, 1, -1])
    N = nrm * np.array([-1, 1, -1])
    part_of = np.zeros(len(pos), np.int64)
    parts = [("body", 0)]
    if cfg["magazine"] is not None:
        parts.append(("magazine", len(parts)))
        part_of[group == cfg["magazine"]] = parts[-1][1]
    if cfg["slide"]:
        parts.append(("slide", len(parts)))
        part_of[np.isin(group, cfg["slide"])] = parts[-1][1]
    if cfg.get("shell") is not None:
        parts.append(("shell", len(parts)))
        part_of[group == cfg["shell"]] = parts[-1][1]
    # triangles grouped by part; a triangle is its first vertex's part
    tp = part_of[tris[:, 0]]
    order = np.argsort(tp, kind="stable")
    tris, tp = tris[order], tp[order]
    ranges = []
    for pname, pid in parts:
        sel = np.nonzero(tp == pid)[0]
        ranges.append((pname, int(sel[0]) * 3 if len(sel) else 0, len(sel) * 3))
    vt = np.dtype([("p", "<f4", 3), ("n", "<f4", 3), ("c", "<f4", 3), ("j", "u1", 4), ("w", "<f4", 4),
                   ("part", "<u4"), ("uv", "<f4", 2)])
    v = np.zeros(len(pos), vt)
    v["p"], v["n"], v["c"], v["part"] = P, N, 1.0, part_of
    v["w"][:, 0] = 1
    v["uv"] = uv
    with open(out, "wb") as f:
        f.write(b"BVR2")
        f.write(struct.pack("<4I", len(pos), len(tris) * 3, 0, len(ranges)))
        f.write(v.tobytes())
        f.write(tris.astype("<u4").tobytes())
        for pname, first, count in ranges:
            f.write(struct.pack("<32sII", pname.encode(), first, count))
        f.write(struct.pack("<II", tex.shape[1], tex.shape[0]))
        f.write(tex.tobytes())
        f.write(struct.pack("<f", cfg["travel"]))
        # where the left hand holds a gun held with both: the pump, or the fore end two fifths along to the muzzle,
        # a little under the bore; flags 1 both hands, 2 a pump
        usedv = np.unique(tris)
        muzzle_z = P[usedv, 2].min()
        bore = P[usedv][P[usedv, 2] < muzzle_z + 0.02].mean(0)
        if cfg.get("pump"):
            fore = P[part_of == [i for n, i in parts if n == "slide"][0]].mean(0)
        else:
            fore = np.array([0.0, bore[1] - 0.03, muzzle_z * 0.42])
        # how the game holds it (tools/game_anim.py, its shot's first frame): the right hand where the game has it
        # on this gun, the left palm on its fore end (when the game's is on the gun: within 6 cm of it), turned as
        # the game's
        right, fore_rot = np.eye(4), np.eye(4)
        if pistol:
            import game_anim
            right, game_fore, turn = game_anim.held(path, pistol, grip)
            if np.linalg.norm(P[usedv] - game_fore, axis=1).min() < 0.06:
                fore, fore_rot = game_fore, turn
        flags = (1 if cfg.get("two") or cfg.get("pump") else 0) | (2 if cfg.get("pump") else 0)
        f.write(struct.pack("<3fI", *fore, flags))
        # the right hand's matrix and the left's turn, column major (BVR2 trailer additions)
        f.write(right.astype("<f4").flatten("F").tobytes())
        f.write(fore_rot.astype("<f4").flatten("F").tobytes())
    used = np.unique(tris)
    print(f"g_{name}: {len(pos)} vertices, {len(tris)} triangles, texture {tex.shape[1]}x{tex.shape[0]}, "
          f"parts {ranges}, grip {np.round(grip, 3)}, bounds {np.round(P[used].min(0), 3)} {np.round(P[used].max(0), 3)}")


if __name__ == "__main__":
    assets, out = sys.argv[1], sys.argv[2]
    files = {}
    for f in sorted(glob.glob(os.path.join(assets, "levels", "*", "fpguns", "*.db"))):
        files.setdefault(os.path.basename(f).lower()[4:-3], f)
    for name, cfg in GUNS.items():
        if name in files:
            convert(name, cfg, files[name], os.path.join(out, f"g_{name}.bvr"), files.get("pst"))
        else:
            print(f"g_{name}: no bg1_{name}.db in {assets}")
