# Blender: converts the VR models in vr_assets/src (glTF) into vr_assets/*.bvr, the format src/vr/models.cpp reads.
#   blender -b -P tools/vr_assets.py -- <vr_assets dir>
#
# Every model ends in metres, right handed, y up:
#   guns: barrel along -z, top +y, origin where the right hand grips (the hand's aim pose), parts "body" and "magazine"
#   avatar: feet at y 0, facing -z, skinned to its joints, parts by mesh (the head is hidden in first person)
#
# .bvr, little endian:
#   "BVR1" u32 vertices u32 indices u32 joints u32 parts
#   vertices: f32 pos[3] f32 normal[3] f32 colour[3] u8 joints[4] f32 weights[4] u32 part   (60 bytes)
#   indices: u32, triangles, grouped by part
#   parts: char name[32] u32 first index u32 index count
#   joints: char name[32] i32 parent f32 inverse bind[16] f32 rest local[16]   (column major, column vectors)
import sys, os, struct, bpy, bmesh
from mathutils import Matrix, Vector

root = os.path.abspath(sys.argv[sys.argv.index("--") + 1])
src = os.path.join(root, "src")

# z up to y up: (x, y, z) -> (x, z, -y)
ZUP = Matrix(((1, 0, 0, 0), (0, 0, 1, 0), (0, -1, 0, 0), (0, 0, 0, 1)))

# per gun: real length in metres, the loose part that is the magazine (-1 none), the part the hand grips
GUNS = {
    "rifle_a": dict(length=0.84, magazine=3, grip=6),   # M4
    "rifle_b": dict(length=0.88, magazine=3, grip=1),   # AK
    "smg_a": dict(length=0.60, magazine=2, grip=6),
    "smg_b": dict(length=0.62, magazine=3, grip=6),
    "sniper": dict(length=1.20, magazine=-1, grip=14),
    "pistol": dict(length=0.20, magazine=-1, grip=9),
    "shotgun": dict(length=1.00, magazine=-1, grip=1),
}
AVATARS = ["avatar_swat", "avatar_adventurer"]


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def colour_of(mat):
    if mat is None:
        return (0.5, 0.5, 0.5)
    if mat.use_nodes:
        for n in mat.node_tree.nodes:
            if n.type == "BSDF_PRINCIPLED":
                c = n.inputs["Base Color"].default_value
                return (c[0], c[1], c[2])
    c = mat.diffuse_color
    return (c[0], c[1], c[2])


def write(path, verts, tris, parts, joints):
    # verts: (pos, normal, colour, joints4, weights4, part); tris: (part, i0, i1, i2)
    tris.sort(key=lambda t: t[0])
    order = []
    ranges = []
    for name, pid in parts:
        first = len(order) * 3
        for t in tris:
            if t[0] == pid:
                order.append(t[1:])
        ranges.append((name, first, len(order) * 3 - first))
    with open(path, "wb") as f:
        f.write(b"BVR1")
        f.write(struct.pack("<4I", len(verts), len(order) * 3, len(joints), len(ranges)))
        for p, n, c, j, w, part in verts:
            f.write(struct.pack("<9f4B4fI", *p, *n, *c, *j, *w, part))
        for t in order:
            f.write(struct.pack("<3I", *t))
        for name, first, count in ranges:
            f.write(struct.pack("<32sII", name.encode()[:31], first, count))
        for name, parent, inv, rest in joints:
            f.write(struct.pack("<32si", name.encode()[:31], parent))
            f.write(struct.pack("<16f", *[inv[r][c] for c in range(4) for r in range(4)]))
            f.write(struct.pack("<16f", *[rest[r][c] for c in range(4) for r in range(4)]))
    print(f"wrote {path}: {len(verts)} vertices, {len(order)} triangles, {len(joints)} joints, parts {ranges}")


def gun(name, cfg):
    reset()
    bpy.ops.import_scene.gltf(filepath=os.path.join(src, name + ".glb"))
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    bpy.ops.object.select_all(action="DESELECT")
    for o in meshes:
        o.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    if len(meshes) > 1:
        bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-4)  # the same loose parts vr_preview.py numbered
    bm.verts.ensure_lookup_table()
    part = {}
    n = 0
    for v in bm.verts:
        if v.index in part:
            continue
        stack = [v]
        part[v.index] = n
        while stack:
            x = stack.pop()
            for e in x.link_edges:
                y = e.other_vert(x)
                if y.index not in part:
                    part[y.index] = n
                    stack.append(y)
        n += 1
    lo = Vector([min(v.co[i] for v in bm.verts) for i in range(3)])
    hi = Vector([max(v.co[i] for v in bm.verts) for i in range(3)])
    scale = cfg["length"] / (hi.x - lo.x)
    # the grip: its part's centre across, its top third up, the hand's palm
    gp = [v.co for v in bm.verts if part[v.index] == cfg["grip"]]
    g_lo = Vector([min(c[i] for c in gp) for i in range(3)])
    g_hi = Vector([max(c[i] for c in gp) for i in range(3)])
    grip = Vector(((g_lo.x + g_hi.x) / 2, (g_lo.y + g_hi.y) / 2, g_lo.z + (g_hi.z - g_lo.z) * 0.75))
    # barrel +x, top +z -> barrel -z, top +y: (x, y, z) -> (-y, z, -x)
    def place(co):
        c = (co - grip) * scale
        return (-c.y, c.z, -c.x)
    def turn(no):
        return (-no.y, no.z, -no.x)
    verts = []
    tris = []
    mats = obj.data.materials
    for f in bm.faces:
        pid = 1 if part[f.verts[0].index] == cfg["magazine"] else 0
        col = colour_of(mats[f.material_index] if f.material_index < len(mats) else None)
        base = len(verts)
        for v in f.verts:
            verts.append((place(v.co), turn(f.normal), col, (0, 0, 0, 0), (1, 0, 0, 0), pid))
        for i in range(1, len(f.verts) - 1):
            tris.append((pid, base, base + i, base + i + 1))
    parts = [("body", 0)] + ([("magazine", 1)] if cfg["magazine"] >= 0 else [])
    write(os.path.join(root, name + ".bvr"), verts, tris, parts, [])


def avatar(name):
    reset()
    bpy.ops.import_scene.gltf(filepath=os.path.join(src, name + ".glb"))
    arm = next(o for o in bpy.context.scene.objects if o.type == "ARMATURE")
    # the skinned meshes only: the packs carry a stray helper sphere
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH" and any(m.type == "ARMATURE" for m in o.modifiers)]
    # rest pose, every action off
    arm.animation_data_clear() if arm.animation_data else None
    for b in arm.pose.bones:
        b.matrix_basis = Matrix.Identity(4)
    bpy.context.view_layer.update()
    bones = list(arm.data.bones)
    index = {b.name: i for i, b in enumerate(bones)}
    W = ZUP @ arm.matrix_world  # armature space to the output's
    joints = []
    for b in bones:
        world = W @ b.matrix_local
        parent = index[b.parent.name] if b.parent else -1
        rest = (W @ b.parent.matrix_local).inverted() @ world if b.parent else world
        joints.append((b.name, parent, world.inverted(), rest))
    verts = []
    tris = []
    parts = []
    for pid, o in enumerate(meshes):
        parts.append((o.name, pid))
        me = o.evaluated_get(bpy.context.evaluated_depsgraph_get()).to_mesh()
        me.calc_loop_triangles()
        M = ZUP @ o.matrix_world
        N = M.to_3x3().inverted().transposed()
        groups = {g.index: g.name for g in o.vertex_groups}
        mats = o.data.materials
        for tri in me.loop_triangles:
            col = colour_of(mats[tri.material_index] if tri.material_index < len(mats) else None)
            base = len(verts)
            for li in tri.loops:
                v = me.vertices[me.loops[li].vertex_index]
                ws = sorted(((g.weight, index.get(groups.get(g.group, ""), -1)) for g in v.groups), reverse=True)
                ws = [(w, j) for w, j in ws if j >= 0 and w > 0][:4]
                total = sum(w for w, _ in ws) or 1
                ws += [(0, 0)] * (4 - len(ws))
                p = M @ v.co
                nrm = (N @ me.loops[li].normal).normalized()
                verts.append((tuple(p), tuple(nrm), col, tuple(j for _, j in ws), tuple(w / total for w, _ in ws), pid))
            tris.append((pid, base, base + 1, base + 2))
    # feet on y 0
    floor = min(v[0][1] for v in verts)
    verts = [((p[0], p[1] - floor, p[2]),) + v[1:] for v in verts for p in [v[0]]]
    joints = [(n, par, inv @ Matrix.Translation((0, floor, 0)), (Matrix.Translation((0, -floor, 0)) @ rest) if par < 0 else rest) for n, par, inv, rest in joints]
    ys = [v[0][1] for v in verts]
    print(f"{name}: height {max(ys) - min(ys):.3f}, feet {min(ys):.3f}")
    write(os.path.join(root, name + ".bvr"), verts, tris, parts, joints)


for g, cfg in GUNS.items():
    gun(g, cfg)
for a in AVATARS:
    avatar(a)
