# The game's own first person animations as glTF, to compare with the VR models: every gun's skeleton, meshes and
# animation clips read straight from its file (assets/levels/*/fpguns/bg1_<gun>.db), nothing recorded:
#   python tools/game_anim.py <assets dir> <vr_assets dir> <out dir>
# writes <out dir>/orig_<gun>.glb, the gun and the arms holding it, skinned, one animation per clip.
#
# The .db sections are found by their type id (8 bytes at the section's start; the table of section offsets at 0x20):
#   skeleton 0000002f9409dcb0, at +0x100: u32 bones at +0x4c, then offsets (from +0x100) of: (i32 parent, u32 flag)
#     per bone (flag 0: not drawn), 4x4 inverse bind matrices (row vectors, D3D order)
#   animations 00000057e5575c55, at +0x100: the state table (names FP_<gun>_S_<nnn>, each naming clips by offset) and
#     at +0x90 the clip offsets (from +0x100). A clip: u32 id, u32 7, u32 loop, 0, -1, u32 frames, 0, 4, 0, u32 events,
#     u32 events offset, then from +0x30 per bone a rotation track and a translation track, in bone order.
#   A track: u8 type, u8 frames, 00, 7f, data. Type 01 is a sequence of n tracks, each over its own frames, sharing
#   their end frames. Rotations (quaternion x y z w, 24 bit floats: a float's top three bytes):
#     0a constant; 11 identity; 0b/0c/0d a polynomial in the frame index of degree 1/2/3 (coefficients highest first,
#     the last the value at the segment's first frame); 0f/10 per frame, base[4] + step[4] * u8[4] / u16[4]
#   Translations (f32 x y z): 02 constant; 09 zero; 03/04/05 polynomials as above; 06 per frame f32[3];
#     07/08 per frame base[3] + step[3] * u8[3] / u16[3]
#   Clips run at 30 frames a second.
# A vertex names its bones by palette slot (offset / 3), which the game fills in its own order: each slot is matched to
# the bone its vertices sit around in the bind pose (checked against the pistol's palette as the game drew it).
import glob
import os
import re
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from export_glb import Gltf, read_bvr  # noqa: E402
import game_guns as gg  # noqa: E402

FPS = 30.0
SKELETON, ANIMS = "0000002f9409dcb0", "00000057e5575c55"
# state numbers to names, in the order a clip takes its name (the first state that plays it)
STATE_NAMES = [("050", "parado"), ("001", "parado"), ("190", "parado"), ("100", "tiro"), ("101", "recarga"),
               ("102", "sacar"), ("125", "fim do tiro")]


def section(db, kind):
    for o in struct.unpack_from("<9I", db, 0x20):
        if db[o:o + 8].hex() == kind:
            return o + 0x100
    raise ValueError("no section " + kind)


def skeleton(db):
    s = section(db, SKELETON)
    nb, op, om = struct.unpack_from("<3I", db, s + 0x4c)
    pf = struct.unpack_from("<%di" % (2 * nb), db, s + op)
    inv = np.frombuffer(db, "<f4", 16 * nb, s + om).reshape(nb, 4, 4).astype(float)
    return list(pf[0::2]), list(pf[1::2]), inv


def f24(d, o, k):
    return np.array([struct.unpack("<f", b"\0" + d[o + 3 * i:o + 3 * i + 3])[0] for i in range(k)])


def f32(d, o, k):
    return np.array(struct.unpack_from("<%df" % k, d, o), float)


SIZE = {0x02: 12, 0x03: 24, 0x04: 36, 0x05: 48, 0x09: 0, 0x0a: 12, 0x0b: 24, 0x0c: 36, 0x0d: 48, 0x11: 0}


def track(db, p):
    """one track at p: (frames n, values n x k), the next offset"""
    t, n = db[p], db[p + 1]
    if db[p + 2] != 0 or db[p + 3] != 0x7f:
        raise ValueError(f"no track at {p:#x}")
    p += 4
    if t == 0x01:
        segs = []
        for _ in range(n):
            v, p = track(db, p)
            segs.append(v if not segs else v[1:])
        v = np.concatenate(segs)
        return v, p
    if t == 0x06:
        size = 12 * n
    elif t in (0x07, 0x08):
        size = (24 + (3 if t == 0x07 else 6) * n + 3) & ~3
    elif t in (0x0f, 0x10):
        size = 24 + (4 if t == 0x0f else 8) * n
    else:
        size = SIZE[t]
    d = db[p:p + size]
    x = np.arange(n, dtype=float)[:, None]
    if t == 0x0a:
        v = np.tile(f24(d, 0, 4), (n, 1))
    elif t == 0x11:
        v = np.tile([0.0, 0, 0, 1], (n, 1))
    elif t in (0x0b, 0x0c, 0x0d):
        c = [f24(d, 12 * i, 4) for i in range(t - 0x09)]
        v = sum(c[j] * x ** (len(c) - 1 - j) for j in range(len(c)))
    elif t in (0x0f, 0x10):
        q = np.frombuffer(d, np.uint8 if t == 0x0f else "<u2", 4 * n, 24).reshape(n, 4)
        v = f24(d, 0, 4) + q * f24(d, 12, 4)
    elif t == 0x02:
        v = np.tile(f32(d, 0, 3), (n, 1))
    elif t == 0x09:
        v = np.zeros((n, 3))
    elif t in (0x03, 0x04, 0x05):
        c = [f32(d, 12 * i, 3) for i in range(t - 0x01)]
        v = sum(c[j] * x ** (len(c) - 1 - j) for j in range(len(c)))
    elif t == 0x06:
        v = np.frombuffer(d, "<f4", 3 * n).reshape(n, 3).astype(float)
    else:
        q = np.frombuffer(d, np.uint8 if t == 0x07 else "<u2", 3 * n, 24).reshape(n, 3)
        v = f32(d, 0, 3) + q * f32(d, 12, 3)
    return v, p + size


def is_clip(db, q):
    if q + 0x30 > len(db):
        return False
    _, ver, loop, zero, minus = struct.unpack_from("<5I", db, q)
    return ver == 7 and loop in (0, 1) and zero == 0 and minus == 0xFFFFFFFF


def bank(db):
    """the animation section's start, its clips' offsets (from the table at +0x90 and every one a state names; a
    gun may play clips the table leaves out) and where the state records end (the first clip)"""
    a = section(db, ANIMS)
    p, tab = a + 0x90, []
    while db[p:p + 4] != b"____":
        tab.append(struct.unpack_from("<I", db, p)[0])
        p += 4
    offs = [t for t in tab if is_clip(db, a + t)]
    end = min(offs)
    x = p
    while x < a + end:
        v = struct.unpack_from("<I", db, x)[0]
        if 0x90 < v < len(db) - a and v % 4 == 0 and v not in offs and is_clip(db, a + v):
            offs.append(v)
            end = min(end, v)
        x += 4
    return a, sorted(offs), end


def clips(db, nb):
    """[(offset, frames, rotations frames x bones x 4, translations frames x bones x 3)]"""
    a, offs, end = bank(db)
    out = []
    for off in offs:
        q = a + off
        n = struct.unpack_from("<I", db, q + 20)[0]
        p, rot, tra = q + 0x30, [], []
        for _ in range(nb):
            for lst in (rot, tra):
                v, p = track(db, p)
                if len(v) < n:
                    v = np.vstack([v, np.tile(v[-1], (n - len(v), 1))])
                lst.append(v[:n])
        r = np.stack(rot, 1)
        r /= np.maximum(np.linalg.norm(r, axis=2, keepdims=True), 1e-9)
        out.append((off, n, r, np.stack(tra, 1)))
    return out, a, end


def clip_names(db, offs, a, end):
    first = end
    names = [(m.start(), m.group(1).decode())
             for m in re.finditer(rb"FP_[A-Za-z0-9]+_S_(\d\d\d)\x00", db[a:a + first])]
    refs = {}
    for i, (o, nm) in enumerate(names):
        e = names[i + 1][0] if i + 1 < len(names) else first
        for x in range(a + o, a + e, 4):
            v = struct.unpack_from("<I", db, x)[0]
            if v in offs:
                refs.setdefault(nm, [])
                if v not in refs[nm]:
                    refs[nm].append(v)
    label = {}
    for state, nm in STATE_NAMES:
        for v in refs.get(state, []):
            if v not in label:
                used = sum(1 for x in label.values() if x.split(" ")[0] == nm.split(" ")[0] and x.startswith(nm))
                label[v] = nm if not used else f"{nm} {used + 1}"
    for state in sorted(refs):
        for v in refs[state]:
            label.setdefault(v, f"estado {state}")
    k = 0
    for v in offs:
        if v not in label:
            k += 1
            label[v] = f"extra {k}"
    return label


def quat_rows(q):
    x, y, z, w = q[..., 0], q[..., 1], q[..., 2], q[..., 3]
    m = np.zeros(q.shape[:-1] + (4, 4))
    m[..., 0, 0] = 1 - 2 * (y * y + z * z)
    m[..., 0, 1] = 2 * (x * y + z * w)
    m[..., 0, 2] = 2 * (x * z - y * w)
    m[..., 1, 0] = 2 * (x * y - z * w)
    m[..., 1, 1] = 1 - 2 * (x * x + z * z)
    m[..., 1, 2] = 2 * (y * z + x * w)
    m[..., 2, 0] = 2 * (x * z + y * w)
    m[..., 2, 1] = 2 * (y * z - x * w)
    m[..., 2, 2] = 1 - 2 * (x * x + y * y)
    m[..., 3, 3] = 1
    return m


def world(par, r, t):
    """row vector world matrices of one frame"""
    w = []
    for b in range(len(par)):
        loc = quat_rows(r[b])
        loc[3, :3] = t[b]
        w.append(loc if par[b] < 0 else loc @ w[par[b]])
    return np.array(w)


def vertices(db):
    ms = gg.meshes(db)
    vb = max(m["rec"] for m in ms) + 0x30
    # the arms: both of them, 0.386 m across (their reach back varies a little from gun to gun)
    arms = [m for m in ms if abs(m["max"][0] - 0.193) < 0.003 and abs(m["min"][0] + 0.193) < 0.003]
    gun = max((m for m in ms if m not in arms), key=lambda m: m["count"])
    groups = []
    for m in [gun] + arms[:1]:
        ix = np.frombuffer(db, np.uint16, m["count"], m["ioff"]).astype(np.int64)
        groups.append(gg.strip(ix))
    n = max(int(g.max()) for g in groups) + 1
    rec = np.frombuffer(db, np.uint8, n * 36, vb).reshape(-1, 36)
    pos = rec[:, 0:12].copy().view(np.float32).reshape(n, 3).astype(float)
    w = rec[:, 12:16].astype(float)
    slot = rec[:, 16:24].copy().view(np.int16).reshape(n, 4) // 3
    pk = rec[:, 24:28].copy().view(np.uint32)[:, 0]
    nrm = np.stack([((pk << 21).astype(np.int32) >> 21) / 1023, ((pk << 10).astype(np.int32) >> 21) / 1023,
                    (pk.astype(np.int32) >> 22) / 511], 1)
    nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-6)
    uv = rec[:, 28:36].copy().view(np.float32).reshape(n, 2)
    return pos, w, slot, nrm, uv, groups


def slot_bones(par, flag, inv, pos, w, slot, used, gun_used, moving=()):
    """palette slot -> bone: the drawn bone whose bind segment (joint to its children) its vertices sit nearest. The
    gun's parts lie too close together for that alone: its body (the slot most of its vertices follow) is the gun's
    root bone (the drawn one at the origin; the body: the slot most of the gun's vertices follow of those that are not
    its moving parts, magazine, slide, shell, given by tools/game_guns.py), its other parts (a revolver's speedloader and rounds hang from the arm, not the gun) and the arms by distance"""
    nb = len(par)
    joint = np.linalg.inv(inv)[:, 3, :3]
    kids = {b: [c for c in range(nb) if par[c] == b] for b in range(nb)}
    dom = slot[used, np.argmax(w[used], 1)]
    slots = sorted(set(dom.tolist()) - {0})
    cost = np.full((len(slots), nb), 1e3)
    for i, k in enumerate(slots):
        p = pos[used][dom == k]
        for b in range(nb):
            if not flag[b]:
                continue
            d = np.full(len(p), np.inf)
            for e in [joint[c] for c in kids[b]] or [joint[b]]:
                s = e - joint[b]
                u = np.clip((p - joint[b]) @ s / max(s @ s, 1e-12), 0, 1)
                d = np.minimum(d, np.linalg.norm(p - (joint[b] + u[:, None] * s), axis=1))
            cost[i, b] = np.median(d)
    gun_dom = slot[gun_used, np.argmax(w[gun_used], 1)]
    counts = {k: int((gun_dom == k).sum()) for k in slots}
    gun_slots = {k for k in slots if counts[k] * 2 > int((dom == k).sum())}
    if gun_slots:
        drawn = [b for b in range(nb) if flag[b]]
        root = min(drawn, key=lambda b: (round(float(np.linalg.norm(joint[b])), 4), -len(kids[b])))
        body = max(gun_slots - set(moving) or gun_slots, key=lambda k: counts[k])
        for i, k in enumerate(slots):
            for b in range(nb):
                if k == body:
                    cost[i, b] = 0.0 if b == root else 1e3
                elif b == root:
                    cost[i, b] = 1e3
    # greedy, then pairwise swaps while they lower the total
    c, rows, cols = cost.copy(), [], []
    for _ in range(min(c.shape)):
        i, j = np.unravel_index(np.argmin(c), c.shape)
        rows.append(i)
        cols.append(j)
        c[i, :], c[:, j] = np.inf, np.inf
    better = True
    while better:
        better = False
        for x in range(len(rows)):
            for y in range(x + 1, len(rows)):
                a, b = rows[x], rows[y]
                if cost[a, cols[y]] + cost[b, cols[x]] < cost[a, cols[x]] + cost[b, cols[y]] - 1e-12:
                    cols[x], cols[y] = cols[y], cols[x]
                    better = True
    out = {0: 0}
    for i, j in zip(rows, cols):
        out[slots[i]] = int(j)
    return out


def export(name, path, arms_tex, out):
    db = open(path, "rb").read()
    par, flag, inv = skeleton(db)
    nb = len(par)
    cl, a, end = clips(db, nb)
    label = clip_names(db, [c[0] for c in cl], a, end)
    pos, w, slot, nrm, uv, groups = vertices(db)
    used = np.unique(np.concatenate([g.reshape(-1) for g in groups]))
    cfg = gg.GUNS.get(name, {})
    moving = [x // 3 for x in ([cfg.get("magazine")] + cfg.get("slide", []) + [cfg.get("shell")]) if x is not None]
    smap = slot_bones(par, flag, inv, pos, w, slot, used, np.unique(groups[0]), moving)
    first_drawn = next(b for b in range(nb) if flag[b])
    bone = np.vectorize(lambda s: smap.get(int(s), first_drawn))(slot)
    fmt = struct.unpack_from("<I", db, 0x18C)[0]
    gun_tex = gg.dxt(db[0x200:], 1 << (fmt >> 20 & 15), 1 << (fmt >> 24 & 15), fmt >> 8 & 0xFF)

    g = Gltf()
    rest = next((c for c in cl if label[c[0]] == "parado"), cl[0])
    jn = []
    for b in range(nb):
        jn.append(g.node({"name": f"osso{b}", "translation": rest[3][0, b].tolist(),
                          "rotation": rest[2][0, b].tolist()}))
    for b in range(nb):
        if par[b] >= 0:
            g.j["nodes"][jn[par[b]]].setdefault("children", []).append(jn[b])
    # the inverse binds are row vector matrices: their rows in order are glTF's column major columns
    ibm = inv.reshape(nb, 16).astype("<f4")
    g.j["skins"] = [{"inverseBindMatrices": g.accessor(ibm, "MAT4", 5126), "joints": jn}]
    meshes = []
    for gi, tris in enumerate(groups):
        arms = gi == 1
        mat = g.material(arms_tex if arms else gun_tex, "braços" if arms else "arma")
        u, local = np.unique(tris, return_inverse=True)
        ww = w[u] / np.maximum(w[u].sum(1, keepdims=True), 1e-6)
        attrs = {"POSITION": g.accessor(pos[u].astype("<f4"), "VEC3", 5126, 34962, True),
                 "NORMAL": g.accessor(nrm[u].astype("<f4"), "VEC3", 5126, 34962),
                 "TEXCOORD_0": g.accessor(np.stack([uv[u, 0], uv[u, 1] % 1.0], 1).astype("<f4"), "VEC2", 5126, 34962),
                 "JOINTS_0": g.accessor(bone[u].astype("<u2"), "VEC4", 5123, 34962),
                 "WEIGHTS_0": g.accessor(ww.astype("<f4"), "VEC4", 5126, 34962)}
        ind = g.accessor(local.reshape(-1).astype("<u4"), "SCALAR", 5125, 34963)
        g.j["meshes"].append({"primitives": [{"attributes": attrs, "indices": ind, "material": mat}]})
        meshes.append(g.node({"name": "braços" if arms else "arma", "mesh": len(g.j["meshes"]) - 1, "skin": 0}))
    # the pistol's palm (tools/game_guns.py) on the gun's main bone at rest, in the VR models' axes
    gun_used = np.unique(groups[0])
    gun_bone = int(np.bincount(bone[gun_used, 0]).argmax())
    wr = world(par, rest[2][0], rest[3][0])
    palm = np.append(gg.PALM, 1) @ inv[gun_bone] @ wr[gun_bone]
    palm = (palm[:3] * np.array([-1, 1, -1])).tolist()

    g.j["animations"] = []
    for off, n, r, t in sorted(cl, key=lambda c: (not label[c[0]].startswith(("parado", "tiro", "recarga")),
                                                  label[c[0]])):
        times = (np.arange(n) / FPS).astype("<f4")
        tin = g.accessor(times, "SCALAR", 5126)
        g.j["accessors"][tin]["min"], g.j["accessors"][tin]["max"] = [0.0], [float(times[-1])]
        t0 = g.accessor(np.zeros(1, "<f4"), "SCALAR", 5126)
        g.j["accessors"][t0]["min"], g.j["accessors"][t0]["max"] = [0.0], [0.0]
        samplers, channels = [], []
        for b in range(nb):
            for path_, vals, kind in (("rotation", r[:, b].copy(), "VEC4"), ("translation", t[:, b], "VEC3")):
                if path_ == "rotation":
                    for f in range(1, n):  # the shorter way round
                        if np.dot(vals[f], vals[f - 1]) < 0:
                            vals[f] = -vals[f]
                still = np.abs(vals - vals[0]).max() < 1e-6
                inp = t0 if still else tin
                samplers.append({"input": inp, "output": g.accessor((vals[:1] if still else vals).astype("<f4"),
                                                                    kind, 5126), "interpolation": "LINEAR"})
                channels.append({"sampler": len(samplers) - 1, "target": {"node": jn[b], "path": path_}})
        g.j["animations"].append({"name": label[off], "samplers": samplers, "channels": channels})
    # the game's space (x left, muzzle +z) to the VR models' (x right, barrel -z): half a turn about y
    root = g.node({"name": "orig_" + name, "rotation": [0.0, 1.0, 0.0, 0.0], "children": [jn[0]] + meshes,
                   "extras": {"palm": palm}})
    g.j["scenes"][0]["nodes"] = [root]
    g.save(out)
    names = [x["name"] for x in g.j["animations"]]
    print(f"orig_{name}: {os.path.getsize(out) // 1024} KB, {nb} bones, {len(cl)} clips {names}")



# --- how the game holds each gun, for the VR models (tools/game_guns.py): the first frame of its shot (the pose it
# fires from; some idle poses carry the gun at rest)

# the left hand of the frame the VR hands were made from (tools/game_hands.py, the pistol's gundump): its bone in the
# pistol's space, column vectors
LEFT_REST = np.array([[0.76568, -0.60315, -0.22335, 0.03818], [0.15557, -0.16318, 0.97423, -0.07961],
                      [-0.62408, -0.78074, -0.03112, -0.12027], [0.0, 0.0, 0.0, 1.0]])
LEFT_ORIGIN = np.array([-0.009, -0.071, 0.018])  # the VR left hand's origin (its palm), in the VR gun's space


def idle_pose(path, clip="tiro"):
    db = open(path, "rb").read()
    par, flag, inv = skeleton(db)
    nb = len(par)
    cl, a, end = clips(db, nb)
    label = clip_names(db, [c[0] for c in cl], a, end)
    c = next((c for c in cl if label[c[0]] == clip), None) or next(c for c in cl if label[c[0]] == "parado")
    w = world(par, c[2][0], c[3][0])
    joint = np.linalg.inv(inv)[:, 3, :3]
    kids = {b: [x for x in range(nb) if par[x] == b] for b in range(nb)}
    root = min([b for b in range(nb) if flag[b]], key=lambda b: (round(float(np.linalg.norm(joint[b])), 4), -len(kids[b])))
    hand = par[root]
    left = next((b for b in range(nb) if b != hand and len(kids[b]) >= 5), None)
    return dict(inv=inv, w=w, root=root, hand=hand, left=left)


def held(path, pistol_path, grip):
    """(right, fore, fore_rot): 4x4 column vector matrices in the VR gun's space (x right, y up, barrel -z, origin
    the grip). right moves the VR right hand (made on the pistol) to where the game's idle pose has it on this gun;
    fore_rot turns the VR left hand to the game's on the fore end, fore where its palm is"""
    g, p = idle_pose(path), idle_pose(pistol_path)
    # this gun's space to the pistol's, the right hands made one (row vectors)
    t = (g["inv"][g["root"]] @ g["w"][g["root"]] @ np.linalg.inv(g["w"][g["hand"]]) @ p["w"][p["hand"]]
         @ np.linalg.inv(p["w"][p["root"]]) @ np.linalg.inv(p["inv"][p["root"]]))
    f = np.diag([-1.0, 1.0, -1.0])

    def into(origin):  # VR (from origin) to the game's space
        m = np.eye(4)
        m[:3, :3], m[:3, 3] = f, f @ origin + grip
        return m

    out = np.eye(4)
    out[:3, :3], out[:3, 3] = f, -f @ grip
    right = out @ np.linalg.inv(t).T @ into(np.zeros(3))
    fore = np.eye(4)
    if g["left"] is not None:
        a = (g["w"][g["left"]] @ np.linalg.inv(g["w"][g["root"]]) @ np.linalg.inv(g["inv"][g["root"]]) @ t).T
        fore = out @ a @ np.linalg.inv(LEFT_REST) @ into(LEFT_ORIGIN)
    rot = np.eye(4)
    rot[:3, :3] = fore[:3, :3]
    return right, fore[:3, 3], rot

if __name__ == "__main__":
    assets, vr, out = sys.argv[1:4]
    os.makedirs(out, exist_ok=True)
    arms_tex = read_bvr(os.path.join(vr, "g_hands.bvr"))[4]
    files = {}
    for f in sorted(glob.glob(os.path.join(assets, "levels", "*", "fpguns", "*.db"))):
        files.setdefault(os.path.basename(f).lower()[4:-3], f)
    for name in gg.GUNS:
        if name in files:
            export(name, files[name], arms_tex, os.path.join(out, f"orig_{name}.glb"))
        else:
            print(f"orig_{name}: no bg1_{name}.db")
