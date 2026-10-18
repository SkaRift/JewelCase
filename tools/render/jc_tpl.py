"""Render a Jewel Case media template from a cartridge model.

Env: CFG (name of an entry in CONFIGS), MO (output folder), Q (samples, default 48)
Writes beauty.png (label = plain white sticker) and mask.png (label = white, rest black).
"""
import bpy, os, json, mathutils
import numpy as np

M = os.environ.get('MODELS', 'models')
CONFIGS = {
    # view: axis index and sign looking FROM; up: axis index for image up
    'wide': dict(model='snes_cartridge', axis=2, sign=1, up=1, label=('plane', None), rect=(0.215, 0.225, 0.80, 0.55), tint=(0.36, 0.35, 0.44)),
    'tall': dict(model='nes_cartridge', axis=1, sign=-1, up=2,
                 label=('uv', 'NESCartridge_baseColor.png', (0, 0, 1179, 2048))),
    'small': dict(model='gameboy_cartridge', axis=2, sign=1, up=1,
                  label=('uv', 'Material.001_baseColor.png', (1174, 188, 1891, 788)),
                  flatten=[('Material.001_normal.png', (1174, 188, 1891, 788), None),
                           ('Material.001_normal.png', (1190, 880, 1880, 1040), (128, 127, 255))],
                  recolor=('Material.001_baseColor.png', (54, 31, 85), (150, 150, 156))),
    'small_dark': dict(model='gameboy_cartridge', axis=2, sign=1, up=1,
                       label=('uv', 'Material.001_baseColor.png', (1174, 188, 1891, 788)),
                       flatten=[('Material.001_normal.png', (1174, 188, 1891, 788), None),
                                ('Material.001_normal.png', (1190, 880, 1880, 1040), (128, 127, 255))],
                       recolor=('Material.001_baseColor.png', (54, 31, 85), (40, 40, 46))),
    'slim': dict(model='gameboy_advance_cartridge', axis=0, sign=1, up=2, label=('object', ['Object_6'])),
    'card': dict(model='nintendo_ds_cartridge', axis=2, sign=1, up=1, label=('object', ['Object_7', 'Object_4'])),
    'card_white': dict(model='nintendo_ds_cartridge', axis=2, sign=1, up=1, label=('object', ['Object_7', 'Object_4']),
                       body_color=(0.80, 0.80, 0.82)),
    'chunky': dict(model='n64_cartridge', stl='N64_Cartridge.stl', axis=1, sign=-1, up=2,
                   label=('plane', None), rect=(0.29, 0.245, 0.71, 0.73), body=(0.11, 0.112, 0.12)),
    'minicard': dict(model='nintendo_switch_cartridge', axis=1, sign=1, up=2, label=('object', ['texture_texture_0'])),
}

name = os.environ['CFG']
cfg = CONFIGS[name]
out = os.environ['MO']
os.makedirs(out, exist_ok=True)
Q = int(os.environ.get('Q', '48'))
RES = int(os.environ.get('RES', '1000'))

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o)
if 'stl' in cfg:
    bpy.ops.wm.stl_import(filepath=f"{M}/{cfg['model']}/{cfg['stl']}")
    bm = bpy.data.materials.new('jc_body')
    bm.use_nodes = True
    bb = bm.node_tree.nodes['Principled BSDF']
    bb.inputs['Base Color'].default_value = (*cfg.get('body', (0.1, 0.1, 0.1)), 1)
    bb.inputs['Roughness'].default_value = 0.55
    for o in bpy.context.scene.objects:
        if o.type == 'MESH':
            o.data.materials.clear()
            o.data.materials.append(bm)
            for poly in o.data.polygons:
                poly.use_smooth = False
else:
    bpy.ops.import_scene.gltf(filepath=f"{M}/{cfg['model']}/scene.gltf")
sc = bpy.context.scene
meshes = [o for o in sc.objects if o.type == 'MESH']


def bbox(objs):
    mn = mathutils.Vector((1e9,) * 3)
    mx = mathutils.Vector((-1e9,) * 3)
    for o in objs:
        for c in o.bound_box:
            w = o.matrix_world @ mathutils.Vector(c)
            mn = mathutils.Vector(map(min, mn, w))
            mx = mathutils.Vector(map(max, mx, w))
    return mn, mx


mn, mx = bbox(meshes)
dims = mx - mn
ctr = (mn + mx) / 2
ax, sg, up = cfg['axis'], cfg['sign'], cfg['up']
size = max(dims[i] for i in range(3) if i != ax)

# --- textures: edit copies in memory -------------------------------------------------------
def img_np(im):
    a = np.array(im.pixels[:], dtype=np.float32).reshape(im.size[1], im.size[0], 4)
    return a  # bottom-up rows


def np_img(im, a):
    im.pixels[:] = a.ravel()
    im.update()


def rect_rows(im, r):
    # r in top-down pixel coords (x0, y0, x1, y1) -> numpy slice of bottom-up array
    H = im.size[1]
    x0, y0, x1, y1 = r
    return slice(H - y1, H - y0), slice(x0, x1)


if 'recolor' in cfg:
    fn, src, dst = cfg['recolor']
    im = bpy.data.images[fn]
    a = img_np(im)
    s = np.array(src, np.float32) / 255
    d = np.array(dst, np.float32) / 255
    # images are sRGB-stored; pixels[] gives stored values. Shift colours near the body colour.
    dist = np.abs(a[..., :3] - s).sum(-1)
    w = np.clip(1 - dist / 0.25, 0, 1)[..., None]
    a[..., :3] = a[..., :3] * (1 - w) + (a[..., :3] - s + d) * w
    np_img(im, np.clip(a, 0, 1))

for fn, r, val in cfg.get('flatten', []):
    im = bpy.data.images[fn]
    a = img_np(im)
    ys, xs = rect_rows(im, r)
    v = np.array(val or (128, 128, 255), np.float32) / 255
    a[ys, xs, :3] = v
    np_img(im, a)

LABEL_WHITE = (0.82, 0.82, 0.82, 1)
kind = cfg['label'][0]
mask_img = None
if kind == 'uv':
    fn, r = cfg['label'][1], cfg['label'][2]
    im = bpy.data.images[fn]
    a = img_np(im)
    ys, xs = rect_rows(im, r)
    lw = np.array(LABEL_WHITE, np.float32)
    lw[:3] = lw[:3] ** (1 / 2.2)  # stored as sRGB
    a[ys, xs] = lw
    np_img(im, a)
    mask_img = bpy.data.images.new('jc_mask', im.size[0], im.size[1], alpha=False)
    mask_img.colorspace_settings.name = 'Non-Color'
    m = np.zeros((im.size[1], im.size[0], 4), np.float32)
    m[..., 3] = 1
    m[ys, xs, :3] = 1
    np_img(mask_img, m)

if 'tint' in cfg:
    for o in meshes:
        for sl in o.material_slots:
            mat = sl.material
            if mat and mat.use_nodes:
                for n in mat.node_tree.nodes:
                    if n.type == 'BSDF_PRINCIPLED' and not n.inputs['Base Color'].is_linked:
                        n.inputs['Base Color'].default_value = (*cfg['tint'], 1)
                        n.inputs['Roughness'].default_value = 0.6

if 'body_color' in cfg:
    for o in meshes:
        for s in o.material_slots:
            mat = s.material
            if not mat or not mat.use_nodes:
                continue
            for n in mat.node_tree.nodes:
                if n.type == 'BSDF_PRINCIPLED' and not n.inputs['Base Color'].is_linked:
                    c = n.inputs['Base Color'].default_value
                    if max(c[:3]) < 0.4:  # the dark body parts
                        n.inputs['Base Color'].default_value = (*cfg['body_color'], 1)

# --- camera ------------------------------------------------------------------------------
view = mathutils.Vector((0, 0, 0))
view[ax] = sg
upv = mathutils.Vector((0, 0, 0))
upv[up] = 1
cd = bpy.data.cameras.new('cam')
cam = bpy.data.objects.new('cam', cd)
sc.collection.objects.link(cam)
sc.camera = cam
cd.type = 'ORTHO'
cd.ortho_scale = size * 1.06
cd.clip_start = 0.001 * size
cd.clip_end = 100 * size
cam.location = ctr + view * size * 3
right = upv.cross(view).normalized()
cam.matrix_world = mathutils.Matrix.Translation(cam.location) @ mathutils.Matrix(
    ((right[0], upv[0], view[0], 0), (right[1], upv[1], view[1], 0), (right[2], upv[2], view[2], 0), (0, 0, 0, 1)))

dg = bpy.context.evaluated_depsgraph_get()

# --- label materials --------------------------------------------------------------------
def principled(name, col, rough=0.45):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    b = mat.node_tree.nodes['Principled BSDF']
    b.inputs['Base Color'].default_value = col
    b.inputs['Roughness'].default_value = rough
    return mat


label_objs = []
lab_mat = principled('jc_label', LABEL_WHITE)
if kind == 'object':
    for n in cfg['label'][1]:
        o = bpy.data.objects[n]
        o.data.materials.clear()
        o.data.materials.append(lab_mat)
        label_objs.append(o)
elif kind == 'plane':
    # find the label recess: cast rays on a grid, take the flat region around the centre
    # region given in normalised image coords (u from left, v from top) by the config, else default
    u0, v0, u1, v1 = cfg.get('rect', (0.30, 0.10, 0.70, 0.62))
    pts = []
    def world_at(u, v):
        return ctr + right * (u - 0.5) * cd.ortho_scale + upv * (0.5 - v) * cd.ortho_scale
    hit_d = []
    for uu in np.linspace(u0, u1, 5):
        for vv in np.linspace(v0, v1, 5):
            p = world_at(uu, vv) + view * size * 2
            ok, loc, nor, idx, ob, mtx = sc.ray_cast(dg, p, -view)
            if ok:
                hit_d.append((loc - ctr).dot(view))
    depth = float(np.max(hit_d)) + size * 0.001
    bpy.ops.mesh.primitive_plane_add(size=1)
    pl = bpy.context.active_object
    c = world_at((u0 + u1) / 2, (v0 + v1) / 2)
    c = c - view * (c - ctr).dot(view) + view * depth
    pl.location = c
    pl.rotation_euler = cam.matrix_world.to_euler()
    pl.scale = ((u1 - u0) * cd.ortho_scale, (v1 - v0) * cd.ortho_scale, 1)
    pl.data.materials.append(lab_mat)
    label_objs.append(pl)
    print('PLANE depth', depth, flush=True)

# --- lights / render settings ----------------------------------------------------------
w = sc.world or bpy.data.worlds.new('w')
sc.world = w
w.use_nodes = True
w.node_tree.nodes['Background'].inputs['Color'].default_value = (0.5, 0.5, 0.52, 1)
w.node_tree.nodes['Background'].inputs['Strength'].default_value = 0.55

def area(name, pos, energy, sz):
    ld = bpy.data.lights.new(name, 'AREA')
    ld.energy = energy
    ld.size = sz
    lo = bpy.data.objects.new(name, ld)
    sc.collection.objects.link(lo)
    lo.location = pos
    lo.rotation_euler = (ctr - pos).to_track_quat('-Z', 'Z' if abs((ctr-pos).normalized()[2]) < 0.9 else 'Y').to_euler()

D = size * 2.5
E = float(os.environ.get('E', '1.0'))
area('key', ctr + view * D + upv * D * 0.9 - right * D * 0.6, 60 * size * size * E, size * 1.6)
area('fill', ctr + view * D - upv * D * 0.3 + right * D * 0.8, 20 * size * size * E, size * 2.0)

r = sc.render
r.engine = 'CYCLES'
sc.cycles.device = 'CPU'
sc.cycles.samples = Q
sc.cycles.use_denoising = True
sc.cycles.max_bounces = 4
r.film_transparent = True
r.resolution_x = r.resolution_y = RES
r.resolution_percentage = 100
sc.view_settings.view_transform = 'Standard'
sc.view_settings.look = 'None'
r.image_settings.file_format = 'PNG'
r.image_settings.color_mode = 'RGBA'
r.threads_mode = 'FIXED'
r.threads = 2

r.filepath = os.path.join(out, 'beauty.png')
bpy.ops.render.render(write_still=True)
print('BEAUTY', flush=True)

# --- mask pass ----------------------------------------------------------------------------
def emit_mat(name, val=None, tex=None):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    o = nt.nodes.new('ShaderNodeOutputMaterial')
    e = nt.nodes.new('ShaderNodeEmission')
    nt.links.new(e.outputs[0], o.inputs[0])
    if tex is not None:
        t = nt.nodes.new('ShaderNodeTexImage')
        t.image = tex
        t.interpolation = 'Closest'
        nt.links.new(t.outputs[0], e.inputs['Color'])
    else:
        e.inputs['Color'].default_value = (val, val, val, 1)
    return mat

black = emit_mat('jc_black', 0.0)
white = emit_mat('jc_white', 1.0)
masked = emit_mat('jc_masked', tex=mask_img) if mask_img else None
uvfn = cfg['label'][1] if kind == 'uv' else None
for o in sc.objects:
    if o.type != 'MESH':
        continue
    for s in o.material_slots:
        mat = s.material
        if o in label_objs:
            s.material = white
            continue
        uses = mat and mat.use_nodes and any(n.type == 'TEX_IMAGE' and n.image and n.image.name == uvfn
                                             for n in mat.node_tree.nodes)
        s.material = masked if (masked and uses) else black
for l in [o for o in sc.objects if o.type == 'LIGHT']:
    l.hide_render = True
w.node_tree.nodes['Background'].inputs['Strength'].default_value = 0
sc.cycles.samples = 16
sc.cycles.use_denoising = False
r.filepath = os.path.join(out, 'mask.png')
bpy.ops.render.render(write_still=True)
print('DONE', flush=True)
