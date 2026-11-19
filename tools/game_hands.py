# The game's own gloved hands for VR, as they hold the pistol: from a dump of the first person draws (BLACK.exe with
# BLACK_VR_DUMPGUN=1 and BLACK_VR_GUN_MODEL=none, holding the pistol, writes gundump\<frame>\ beside it) to
# vr_assets/g_hands.bvr. Plain Python 3 with numpy:
#   python tools/game_hands.py <gundump\<frame> dir> <vr_assets dir>
# The dump holds the arms' draw and the gun's (36 byte vertices as in tools/game_guns.py) and the bone palette they
# were drawn with (vertex shader constants: bone b's rows at c[60 + b]). The arms posed by it, taken into the
# pistol's own space and from there into the VR gun's (barrel -z, origin at the palm), give:
#   part "right": the right hand and forearm, drawn with the gun (or alone at the right hand's grip)
#   part "left": the left hand and forearm, around its palm, drawn at the left hand's grip
import glob
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from game_guns import strip  # noqa: E402

GUN_BODY = 108                       # the pistol's frame bone
PISTOL_GRIP = np.array([0.0, -0.012, -0.068])  # tools/game_guns.py's palm for the pistol, in its space
LEFT = {12, 15, 18} | set(range(21, 64, 3))   # forearm (its sleeve, held rigid to the hand), hand and fingers: the support hand
RIGHT = {69, 72} | set(range(75, 106, 3)) | {123, 126, 129}
LEFT_PALM = 18


def draw(prefix):
    info = open(prefix + "info.txt").read().split("\n")[0].split()
    kv = {info[i]: info[i + 1] for i in range(0, len(info) - 1, 2)}
    vb = np.fromfile(prefix + "vb0.bin", np.uint8)
    rec = vb[:len(vb) // 36 * 36].reshape(-1, 36)
    n = len(rec)
    pos = rec[:, 0:12].copy().view(np.float32).reshape(n, 3)
    w = rec[:, 12:16].astype(np.float32) / 255
    bone = rec[:, 16:24].copy().view(np.int16).reshape(n, 4).astype(np.int64)
    pk = rec[:, 24:28].copy().view(np.uint32)[:, 0]
    nrm = np.stack([((pk << 21).astype(np.int32) >> 21) / 1023, ((pk << 10).astype(np.int32) >> 21) / 1023,
                    (pk.astype(np.int32) >> 22) / 511], 1)
    uv = rec[:, 28:36].copy().view(np.float32).reshape(n, 2)
    ix = np.fromfile(prefix + "indices16.bin", np.uint16).astype(np.int64) + int(kv["base_vertex"])
    vsc = np.fromfile(prefix + "vsc.bin", np.float32).reshape(192, 4)
    return dict(pos=pos, w=w, bone=bone, nrm=nrm, uv=uv, tris=strip(ix), vsc=vsc, count=int(kv["count"]))


def bmp(path):
    b = open(path, "rb").read()
    w = int.from_bytes(b[18:22], "little", signed=True)
    h = int.from_bytes(b[22:26], "little", signed=True)
    px = np.frombuffer(b, np.uint8, w * abs(h) * 4, 54).reshape(abs(h), w, 4)[:, :, [2, 1, 0, 3]]
    return px if h < 0 else px[::-1]


TRIGGER = 120  # the pistol's trigger bone: the right finger nearest it is the trigger finger
THUMBS = {57, 60, 63, 123, 126, 129}
CHAINS = {"R": [[75, 78, 81], [84, 87, 90], [93, 96, 99], [102, 105], [123, 126, 129]],
          "L": [[21, 24, 27], [30, 33, 36], [39, 42, 45], [48, 51, 54], [57, 60, 63]]}


def finger_joints(P, w, bone, group, palm, gun):
    """Joints for the fingers, in the posed (VR) space: per hand a root at the palm, then each finger's segments
    in a chain, local x the axis it curls about (positive closes the hand). Names: R_root, R_trigger_0..,
    R_finger_.., R_thumb_..; L_ likewise. Returns (name, parent, inverse bind, rest local) and bone -> joint."""
    hands = {"R": (RIGHT, 72, np.zeros(3)), "L": (LEFT, LEFT_PALM, palm)}
    # bones that share a vertex: the skeleton's links
    links = {}
    for i in range(len(w)):
        bs = [int(bone[i, k]) for k in range(4) if w[i, k] > 0.15]
        for a in bs:
            for b in bs:
                if a != b:
                    links.setdefault((a, b), []).append(P[i])
    # the pistol's trigger in the same space
    gv = gun["pos"][gun["bone"][:, 0] == TRIGGER]
    trigger = (gv.mean(0) - PISTOL_GRIP) * np.array([-1, 1, -1]) if len(gv) else np.zeros(3)
    joints, joint_of = [], {}

    def add(name, parent, glob_m, origin):
        local_parent = np.eye(4) if parent < 0 else joints[parent][4]
        g = glob_m.copy()
        g[:3, 3] -= origin
        joints.append([name, parent, np.linalg.inv(g), np.linalg.inv(local_parent) @ g, g])
        return len(joints) - 1

    for side, (groups, palm_bone, origin) in hands.items():
        centre = P[group == palm_bone].mean(0)
        root = add(f"{side}_root", -1, np.eye(4), origin * 0)
        joints[root][2] = np.eye(4)
        joints[root][3] = np.eye(4)
        joints[root][4] = np.eye(4)
        joint_of[side] = root
        for b in groups:
            joint_of[int(b)] = root
        # finger chains: three bones each, in palette order from the knuckle (the right little finger has two)
        chains = CHAINS[side]
        tips = [P[group == c[-1]][np.argmax(np.linalg.norm(P[group == c[-1]] - centre, axis=1))] for c in chains]
        # the index finger: on the right the one at the pistol's trigger, on the left the one beside the thumb
        if side == "R":
            index = int(np.argmin([np.linalg.norm(t - trigger) for t in tips]))
        else:
            thumb = P[group == next(c for c in chains if c[0] in THUMBS)[0]].mean(0)
            index = int(np.argmin([np.linalg.norm(P[group == c[0]].mean(0) - thumb) if c[0] not in THUMBS else 9
                                   for c in chains]))
        # the hinge every finger bends about: across the knuckles, from the index's to the little finger's
        def pivot_of(prev, b):
            pts = links.get((prev, b))
            return np.mean(pts, 0) if pts else P[group == b][np.argmin(np.linalg.norm(P[group == b] - centre, axis=1))]
        fingers = [c for c in chains if c[0] not in THUMBS]
        bases = [pivot_of(palm_bone, c[0]) for c in fingers]
        index_base = pivot_of(palm_bone, chains[index][0])
        far = max(bases, key=lambda p: np.linalg.norm(p - index_base))
        across = far - index_base
        across /= np.linalg.norm(across)
        # the palm's face: across the palm's points the least spread way, towards where the curled fingers lie
        pp = P[group == palm_bone] - centre
        normal = np.linalg.svd(pp, full_matrices=False)[2][2]
        tips_mean = np.mean([P[group == c[-1]].mean(0) for c in fingers], 0)
        if np.dot(tips_mean - centre, normal) < 0:
            normal = -normal
        target = centre + normal * 0.06  # where a closing finger heads
        for ci, chain in enumerate(chains):
            kind = "thumb" if chain[0] in THUMBS else "index" if ci == index else "finger"
            parent, prev = root, palm_bone
            for si, b in enumerate(chain):
                pivot = pivot_of(prev, b)
                seg = P[group == b].mean(0)
                d = seg - pivot
                d /= max(np.linalg.norm(d), 1e-6)
                # fingers bend about the knuckles' line (made square to the segment), the sign that brings the
                # segment towards the palm closing the hand. The thumb stands about along the palm's normal, so a
                # hinge from the normal is ill defined: it curls over towards the fingertips, as round a grip
                goal = target if kind != "thumb" else tips_mean
                hinge = across if kind != "thumb" else np.cross(d, tips_mean - pivot)
                axis = hinge - d * np.dot(hinge, d)
                if np.linalg.norm(axis) < 1e-6:
                    axis = np.cross(d, normal)
                axis /= np.linalg.norm(axis)
                turned = d * np.cos(0.3) + np.cross(axis, d) * np.sin(0.3)
                if np.linalg.norm(pivot + turned * 0.03 - goal) > np.linalg.norm(pivot + d * 0.03 - goal):
                    axis = -axis
                y = np.cross(d, axis)
                m = np.eye(4)
                m[:3, 0], m[:3, 1], m[:3, 2], m[:3, 3] = axis, y, d, pivot
                parent = add(f"{side}_{kind}_{ci}_{si}", parent, m, origin)
                joint_of[int(b)] = parent
                prev = b
    return [(n, p, ib, r) for n, p, ib, r, _ in joints], joint_of


def main(dump, out):
    draws = [draw(p[:-8]) for p in sorted(glob.glob(os.path.join(dump, "*_info.txt")))]
    arms = next(d for d in draws if d["count"] < 2000)
    gun = next(d for d in draws if d is not arms)
    vsc = arms["vsc"]

    def bone(b):
        m = np.eye(4)
        m[:3] = vsc[60 + b:63 + b]
        return m

    # posed: each vertex by its bones' weights
    P = np.zeros((len(arms["pos"]), 3))
    N = np.zeros_like(P)
    for k in range(4):
        for b in np.unique(arms["bone"][:, k]):
            sel = arms["bone"][:, k] == b
            m = bone(int(b))
            P[sel] += arms["w"][sel, k:k + 1] * (arms["pos"][sel] @ m[:3, :3].T + m[:3, 3])
            N[sel] += arms["w"][sel, k:k + 1] * (arms["nrm"][sel] @ m[:3, :3].T)
    # into the pistol's space, then the VR gun's
    g = np.eye(4)
    g[:3] = gun["vsc"][60 + GUN_BODY:63 + GUN_BODY]  # the gun draw's own palette
    inv = np.linalg.inv(g)
    P = P @ inv[:3, :3].T + inv[:3, 3]
    N = N @ inv[:3, :3].T
    N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-6)
    flip = np.array([-1, 1, -1])
    P = (P - PISTOL_GRIP) * flip
    N = N * flip
    group = arms["bone"][np.arange(len(P)), arms["w"].argmax(1)]
    # the left hand around its palm (the left grip's place)
    palm = P[group == LEFT_PALM].mean(0)
    tris = arms["tris"]
    keep = []
    for name, groups in (("right", RIGHT), ("left", LEFT)):
        t = tris[np.isin(group[tris[:, 0]], list(groups)) & np.isin(group[tris[:, 1]], list(groups))
                 & np.isin(group[tris[:, 2]], list(groups))]
        keep.append((name, t))
    used = np.unique(np.concatenate([t for _, t in keep]).ravel())
    remap = -np.ones(len(P), np.int64)
    remap[used] = np.arange(len(used))
    part = np.zeros(len(P), np.int64)
    part[np.isin(group, list(LEFT))] = 1
    pos = P.copy()
    pos[part == 1] -= palm
    joints, joint_of = finger_joints(P, arms["w"], arms["bone"], group, palm, gun)
    vt = np.dtype([("p", "<f4", 3), ("n", "<f4", 3), ("c", "<f4", 3), ("j", "u1", 4), ("w", "<f4", 4),
                   ("part", "<u4"), ("uv", "<f4", 2)])
    v = np.zeros(len(used), vt)
    v["p"], v["n"], v["c"], v["part"], v["uv"] = pos[used], N[used], 1.0, part[used], arms["uv"][used]
    # the game's weights, onto the joints (a bone of no joint of its own follows its hand's root)
    for k in range(4):
        b = arms["bone"][used, k]
        root = np.where(part[used] == 1, joint_of["L"], joint_of["R"])
        v["j"][:, k] = [joint_of.get(int(x), r) for x, r in zip(b, root)]
        v["w"][:, k] = arms["w"][used, k]
    v["w"] /= np.maximum(v["w"].sum(1, keepdims=True), 1e-6)
    tex_path = [p for p in glob.glob(os.path.join(dump, "tex_*.bmp")) if bmp(p).shape[0] == 512]
    tex = bmp(tex_path[0]) if tex_path else np.full((1, 1, 4), 128, np.uint8)
    idx, ranges, first = [], [], 0
    for name, t in keep:
        idx.append(remap[t].ravel())
        ranges.append((name, first, t.size))
        first += t.size
    idx = np.concatenate(idx)
    with open(os.path.join(out, "g_hands.bvr"), "wb") as f:
        f.write(b"BVR2")
        f.write(struct.pack("<4I", len(v), len(idx), len(joints), len(ranges)))
        f.write(v.tobytes())
        f.write(idx.astype("<u4").tobytes())
        for name, a, c in ranges:
            f.write(struct.pack("<32sII", name.encode(), a, c))
        for name, parent, inv_bind, rest in joints:
            f.write(struct.pack("<32si", name.encode(), parent))
            f.write(struct.pack("<16f", *inv_bind.T.ravel()))   # column major
            f.write(struct.pack("<16f", *rest.T.ravel()))
        f.write(struct.pack("<II", tex.shape[1], tex.shape[0]))
        f.write(np.ascontiguousarray(tex).tobytes())
        f.write(struct.pack("<f", 0.0))
    print("joints:", " ".join(f"{n}<{p}" for n, p, _, _ in joints))
    print(f"g_hands: {len(v)} vertices, parts {ranges}, left palm {np.round(palm, 3)}, "
          f"right bounds {np.round(pos[used][part[used] == 0].min(0), 3)} {np.round(pos[used][part[used] == 0].max(0), 3)}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
