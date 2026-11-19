# The VR models (vr_assets/*.bvr, from tools/game_guns.py and tools/game_hands.py) as glTF binaries, one per model,
# for viewing outside the game, and a page to view them (tools/vitrine.html as a template):
#   python tools/export_glb.py <vr_assets dir> <out dir>
# Each part of a gun is a node of its own (body, magazine, slide, shell), so a viewer can move it as the game does;
# the gun's facts ride in the root node's extras: the slide's travel, the fore end where the left hand holds it, two
# handed, a pump. The hands are one skinned mesh per hand on the finger joints (local x the axis a finger curls about).
# Model space: metres, y up, the barrel along -z, the origin where the right hand's palm holds the gun.
import base64
import glob
import json
import os
import struct
import sys
import zlib

import numpy as np

VT = np.dtype([("p", "<f4", 3), ("n", "<f4", 3), ("c", "<f4", 3), ("j", "u1", 4), ("w", "<f4", 4), ("part", "<u4"),
               ("uv", "<f4", 2)])


def read_bvr(path):
    b = open(path, "rb").read()
    version = b[:4]
    nv, ni, nj, npart = struct.unpack_from("<4I", b, 4)
    o = 20
    if version == b"BVR2":
        v = np.frombuffer(b, VT, nv, o)
        o += nv * 68
    else:
        v1 = np.frombuffer(b, VT.descr and np.dtype(VT.descr[:-1]), nv, o)
        v = np.zeros(nv, VT)
        for k in v1.dtype.names:
            v[k] = v1[k]
        o += nv * 60
    idx = np.frombuffer(b, "<u4", ni, o).copy()
    o += ni * 4
    parts = []
    for _ in range(npart):
        nm, first, count = struct.unpack_from("<32sII", b, o)
        o += 40
        parts.append((nm.split(b"\0")[0].decode(), first, count))
    joints = []
    for _ in range(nj):
        nm = struct.unpack_from("<32s", b, o)[0].split(b"\0")[0].decode()
        o += 32
        parent = struct.unpack_from("<i", b, o)[0]
        o += 4
        inv = np.array(struct.unpack_from("<16f", b, o)).reshape(4, 4).T
        o += 64
        rest = np.array(struct.unpack_from("<16f", b, o)).reshape(4, 4).T
        o += 64
        joints.append((nm, parent, inv, rest))
    tex, extras = None, {}
    if version == b"BVR2" and o + 8 <= len(b):
        tw, th = struct.unpack_from("<II", b, o)
        o += 8
        tex = np.frombuffer(b, np.uint8, tw * th * 4, o).reshape(th, tw, 4)
        o += tw * th * 4
        if o + 4 <= len(b):
            extras["slide_travel"] = struct.unpack_from("<f", b, o)[0]
            o += 4
        if o + 16 <= len(b):
            fx, fy, fz, flags = struct.unpack_from("<3fI", b, o)
            extras.update(fore=[fx, fy, fz], two_handed=bool(flags & 1), pump=bool(flags & 2))
            o += 16
        if o + 128 <= len(b):
            extras["hand"] = list(struct.unpack_from("<16f", b, o))
            extras["fore_rot"] = list(struct.unpack_from("<16f", b, o + 64))
    return v, idx, parts, joints, tex, extras


def png(rgba):
    h, w = rgba.shape[:2]
    raw = b"".join(b"\0" + rgba[y].tobytes() for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


class Gltf:
    def __init__(self):
        self.j = {"asset": {"version": "2.0", "generator": "Black VR export_glb.py"}, "scene": 0,
                  "scenes": [{"nodes": []}], "nodes": [], "meshes": [], "accessors": [], "bufferViews": [],
                  "buffers": [], "materials": []}
        self.bin = bytearray()

    def view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        bv = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            bv["target"] = target
        self.bin += data
        self.j["bufferViews"].append(bv)
        return len(self.j["bufferViews"]) - 1

    def accessor(self, arr, kind, comp, target=None, minmax=False):
        a = {"bufferView": self.view(arr.tobytes(), target), "componentType": comp, "count": len(arr), "type": kind}
        if minmax:
            a["min"] = arr.min(0).tolist()
            a["max"] = arr.max(0).tolist()
        self.j["accessors"].append(a)
        return len(self.j["accessors"]) - 1

    def material(self, tex, name):
        m = {"name": name, "pbrMetallicRoughness": {"metallicFactor": 0.3, "roughnessFactor": 0.6},
             "doubleSided": True}
        if tex is not None:
            img = self.view(png(tex))
            self.j.setdefault("images", []).append({"bufferView": img, "mimeType": "image/png"})
            self.j.setdefault("samplers", []).append({"magFilter": 9729, "minFilter": 9987})
            self.j.setdefault("textures", []).append({"source": len(self.j["images"]) - 1, "sampler": 0})
            m["pbrMetallicRoughness"]["baseColorTexture"] = {"index": len(self.j["textures"]) - 1}
        self.j["materials"].append(m)
        return len(self.j["materials"]) - 1

    def node(self, n):
        self.j["nodes"].append(n)
        return len(self.j["nodes"]) - 1

    def save(self, path):
        self.j["buffers"] = [{"byteLength": len(self.bin)}]
        js = json.dumps(self.j, separators=(",", ":")).encode()
        js += b" " * (-len(js) % 4)
        bn = bytes(self.bin) + b"\0" * (-len(self.bin) % 4)
        out = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(bn))
        out += struct.pack("<II", len(js), 0x4E4F534A) + js + struct.pack("<II", len(bn), 0x004E4942) + bn
        open(path, "wb").write(out)


def mesh_of(g, v, tris, mat, skinned):
    used, local = np.unique(tris, return_inverse=True)
    vv = v[used]
    attrs = {"POSITION": g.accessor(vv["p"].astype("<f4"), "VEC3", 5126, 34962, True),
             "NORMAL": g.accessor(vv["n"].astype("<f4"), "VEC3", 5126, 34962),
             # the game's uv: v counted down from 1 (D3D wraps it): glTF's runs top down from 0
             "TEXCOORD_0": g.accessor(np.stack([vv["uv"][:, 0], vv["uv"][:, 1] % 1.0], 1).astype("<f4"), "VEC2",
                                      5126, 34962)}
    if skinned:
        attrs["JOINTS_0"] = g.accessor(vv["j"].astype("<u2"), "VEC4", 5123, 34962)
        w = vv["w"] / np.maximum(vv["w"].sum(1, keepdims=True), 1e-6)
        attrs["WEIGHTS_0"] = g.accessor(w.astype("<f4"), "VEC4", 5126, 34962)
    ind = g.accessor(local.reshape(-1).astype("<u4"), "SCALAR", 5125, 34963)  # every corner, not a row per triangle
    g.j["meshes"].append({"primitives": [{"attributes": attrs, "indices": ind, "material": mat}]})
    return len(g.j["meshes"]) - 1


def gun_glb(name, v, idx, parts, tex, extras, out):
    g = Gltf()
    mat = g.material(tex, name)
    children = []
    for pname, first, count in parts:
        if not count:
            continue
        tris = idx[first:first + count].reshape(-1, 3)
        children.append(g.node({"name": pname, "mesh": mesh_of(g, v, tris, mat, False)}))
    root = g.node({"name": name, "children": children, "extras": extras})
    g.j["scenes"][0]["nodes"] = [root]
    g.save(out)


def hands_glb(name, v, idx, parts, joints, tex, out):
    g = Gltf()
    mat = g.material(tex, name)
    jn = []
    for jname, parent, inv, rest in joints:
        t = rest[:3, 3]
        rot = rest[:3, :3]
        # rest locals are rigid: quaternion of the 3x3
        m = rot
        tr = np.trace(m)
        if tr > 0:
            s = np.sqrt(tr + 1) * 2
            q = [(m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, s / 4]
        else:
            i = int(np.argmax(np.diag(m)))
            j, k = (i + 1) % 3, (i + 2) % 3
            s = np.sqrt(1 + m[i, i] - m[j, j] - m[k, k]) * 2
            q = [0, 0, 0, 0]
            q[i] = s / 4
            q[j] = (m[j, i] + m[i, j]) / s
            q[k] = (m[k, i] + m[i, k]) / s
            q[3] = (m[k, j] - m[j, k]) / s
        jn.append(g.node({"name": jname, "translation": t.tolist(), "rotation": [float(x) for x in q]}))
    for ji, (jname, parent, inv, rest) in enumerate(joints):
        if parent >= 0:
            g.j["nodes"][jn[parent]].setdefault("children", []).append(jn[ji])
    ibm = np.stack([inv.T for _, _, inv, _ in joints]).astype("<f4")  # column major
    skin_acc = g.accessor(ibm.reshape(-1, 16), "MAT4", 5126)
    g.j["skins"] = [{"inverseBindMatrices": skin_acc, "joints": jn}]
    roots = [jn[i] for i, (_, p, _, _) in enumerate(joints) if p < 0]
    nodes = []
    for pname, first, count in parts:
        tris = idx[first:first + count].reshape(-1, 3)
        nodes.append(g.node({"name": pname, "mesh": mesh_of(g, v, tris, mat, True), "skin": 0}))
    root = g.node({"name": name, "children": roots + nodes})
    g.j["scenes"][0]["nodes"] = [root]
    g.save(out)


if __name__ == "__main__":
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for path in sorted(glob.glob(os.path.join(src, "g_*.bvr"))):
        name = os.path.basename(path)[:-4]
        v, idx, parts, joints, tex, extras = read_bvr(path)
        dest = os.path.join(out, name + ".glb")
        if joints:
            hands_glb(name, v, idx, parts, joints, tex, dest)
        else:
            gun_glb(name, v, idx, parts, tex, extras, dest)
        print(f"{dest}: {os.path.getsize(dest) // 1024} KB, parts {[p[0] for p in parts]}, {extras}")
