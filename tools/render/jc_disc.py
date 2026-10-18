"""Render the CD from the jewel case blend, top down, label side. Env: MO, Q, RES, SIDE (label|data)"""
import bpy, os, mathutils

out = os.environ['MO']
os.makedirs(out, exist_ok=True)
Q = int(os.environ.get('Q', '40'))
RES = int(os.environ.get('RES', '1000'))
bpy.ops.wm.open_mainfile(filepath=os.path.join(os.environ.get('MODELS', 'models'), 'cd_jewel_case.blend'))
sc = bpy.context.scene
for o in list(sc.objects):
    if o.name != 'CD' and o.type in ('MESH', 'LIGHT', 'CAMERA', 'EMPTY', 'CURVE'):
        o.hide_render = True
cd_o = bpy.data.objects['CD']
mn = mathutils.Vector((1e9,) * 3)
mx = mathutils.Vector((-1e9,) * 3)
for c in cd_o.bound_box:
    w = cd_o.matrix_world @ mathutils.Vector(c)
    mn = mathutils.Vector(map(min, mn, w))
    mx = mathutils.Vector(map(max, mx, w))
ctr = (mn + mx) / 2
dims = mx - mn
print('CD dims', tuple(dims), flush=True)
thin = min(range(3), key=lambda i: dims[i])
oth = [i for i in range(3) if i != thin]
size = max(dims[i] for i in oth)
view = mathutils.Vector((0, 0, 0)); view[thin] = float(os.environ.get('SG', '1'))
upv = mathutils.Vector((0, 0, 0)); upv[oth[1]] = 1
right = upv.cross(view).normalized()
camd = bpy.data.cameras.new('jc')
cam = bpy.data.objects.new('jc', camd)
sc.collection.objects.link(cam)
sc.camera = cam
camd.type = 'ORTHO'
camd.ortho_scale = size * 1.04
cam.location = ctr + view * size * 3
cam.matrix_world = mathutils.Matrix.Translation(cam.location) @ mathutils.Matrix(
    ((right[0], upv[0], view[0], 0), (right[1], upv[1], view[1], 0), (right[2], upv[2], view[2], 0), (0, 0, 0, 1)))

lab = bpy.data.materials.new('jc_label')
lab.use_nodes = True
lab.node_tree.nodes['Principled BSDF'].inputs['Base Color'].default_value = (0.82, 0.82, 0.82, 1)
lab.node_tree.nodes['Principled BSDF'].inputs['Roughness'].default_value = 0.5
slots = cd_o.material_slots
print('slots', [s.material.name for s in slots], flush=True)
LABEL_SLOT = int(os.environ.get('LS', '0'))
orig = [s.material for s in slots]
slots[LABEL_SLOT].material = lab

w = sc.world or bpy.data.worlds.new('w')
sc.world = w
w.use_nodes = True
bg = w.node_tree.nodes.get('Background')
for n in list(w.node_tree.nodes):
    if n.type not in ('BACKGROUND', 'OUTPUT_WORLD'):
        w.node_tree.nodes.remove(n)
bg = [n for n in w.node_tree.nodes if n.type == 'BACKGROUND'][0]
outn = [n for n in w.node_tree.nodes if n.type == 'OUTPUT_WORLD'][0]
w.node_tree.links.new(bg.outputs[0], outn.inputs[0])
bg.inputs['Color'].default_value = (0.5, 0.5, 0.52, 1)
bg.inputs['Strength'].default_value = 0.55
D = size * 2.5
def area(name, pos, energy, sz):
    ld = bpy.data.lights.new(name, 'AREA')
    ld.energy = energy
    ld.size = sz
    lo = bpy.data.objects.new(name, ld)
    sc.collection.objects.link(lo)
    lo.location = pos
    lo.rotation_euler = (ctr - pos).to_track_quat('-Z', 'Z').to_euler()
area('key', ctr + view * D + upv * 0.9 * D - right * 0.6 * D, 60 * size * size, size * 1.6)
area('fill', ctr + view * D - upv * 0.3 * D + right * 0.8 * D, 20 * size * size, size * 2)

r = sc.render
r.engine = 'CYCLES'
sc.cycles.device = 'CPU'
sc.cycles.samples = Q
sc.cycles.use_denoising = True
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

def emit(v):
    m = bpy.data.materials.new('e%g' % v)
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    o = nt.nodes.new('ShaderNodeOutputMaterial')
    e = nt.nodes.new('ShaderNodeEmission')
    e.inputs['Color'].default_value = (v, v, v, 1)
    nt.links.new(e.outputs[0], o.inputs[0])
    return m
for i, s in enumerate(slots):
    s.material = emit(1.0 if i == LABEL_SLOT else 0.0)
for o in sc.objects:
    if o.type == 'LIGHT':
        o.hide_render = True
bg.inputs['Strength'].default_value = 0
sc.cycles.samples = 16
sc.cycles.use_denoising = False
r.filepath = os.path.join(out, 'mask.png')
bpy.ops.render.render(write_still=True)
print('DONE', flush=True)
