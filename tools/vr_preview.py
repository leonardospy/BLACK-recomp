# Blender: renders each gun in vr_assets/src from the side, its loose parts in different colours, with its bounds,
# so the barrel, grip and magazine can be told apart.
#   blender -b -P tools/vr_preview.py -- <src dir> <out dir>
import sys, os, bpy, bmesh, colorsys
from mathutils import Vector

src, out = [os.path.abspath(p) for p in sys.argv[sys.argv.index("--") + 1:][:2]]
os.makedirs(out, exist_ok=True)

def clear():
    bpy.ops.wm.read_factory_settings(use_empty=True)

for name in sorted(os.listdir(src)):
    if not name.endswith(".glb") or name.startswith("avatar"):
        continue
    clear()
    bpy.ops.import_scene.gltf(filepath=os.path.join(src, name))
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    bpy.ops.object.select_all(action="DESELECT")
    for o in meshes:
        o.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    if len(meshes) > 1:
        bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    # loose parts, each its own material colour
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-4)  # flat shading splits every face apart
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
    info = []
    for p in range(n):
        pts = [v.co for v in bm.verts if part[v.index] == p]
        lo = Vector((min(c.x for c in pts), min(c.y for c in pts), min(c.z for c in pts)))
        hi = Vector((max(c.x for c in pts), max(c.y for c in pts), max(c.z for c in pts)))
        info.append((p, len(pts), lo, hi))
    mats = []
    for p in range(n):
        m = bpy.data.materials.new(f"p{p}")
        r, g, b = colorsys.hsv_to_rgb(p / max(n, 1), 0.8, 0.9)
        m.diffuse_color = (r, g, b, 1)
        mats.append(m)
    obj.data.materials.clear()
    for m in mats:
        obj.data.materials.append(m)
    for f in bm.faces:
        f.material_index = part[f.verts[0].index]
    bm.to_mesh(obj.data)
    lo = Vector([min(v.co[i] for v in obj.data.vertices) for i in range(3)])
    hi = Vector([max(v.co[i] for v in obj.data.vertices) for i in range(3)])
    size = hi - lo
    with open(os.path.join(out, name[:-4] + ".txt"), "w") as f:
        f.write(f"bounds {tuple(round(c, 4) for c in lo)} {tuple(round(c, 4) for c in hi)} size {tuple(round(c, 4) for c in size)}\n")
        for p, cnt, a, b in sorted(info, key=lambda i: -i[1])[:16]:
            r, g, bb = mats[p].diffuse_color[:3]
            f.write(f"part {p} verts {cnt} colour ({r:.2f},{g:.2f},{bb:.2f}) min {tuple(round(c, 4) for c in a)} max {tuple(round(c, 4) for c in b)}\n")
    # three orthographic views: from +X, from -Y (front), from +Z (top)
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.color_type = "MATERIAL"
    scene.render.resolution_x = 640
    scene.render.resolution_y = 400
    world = bpy.data.worlds.new("w")
    scene.world = world
    centre = (lo + hi) / 2
    span = max(size) * 1.2
    for tag, direction, up in (("side_x", Vector((1, 0, 0)), "Z"), ("side_y", Vector((0, -1, 0)), "Z"),
                               ("top_z", Vector((0, 0, 1)), "Y")):
        cam_data = bpy.data.cameras.new(tag)
        cam_data.type = "ORTHO"
        cam_data.ortho_scale = span
        cam = bpy.data.objects.new(tag, cam_data)
        scene.collection.objects.link(cam)
        cam.location = centre + direction * span * 2
        cam.rotation_euler = (-direction).to_track_quat("-Z", up).to_euler()
        scene.camera = cam
        scene.render.filepath = os.path.join(out, f"{name[:-4]}_{tag}.png")
        bpy.ops.render.render(write_still=True)
