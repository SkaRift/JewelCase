# Template renders

These scripts render the cartridge, card and disc templates in `assets/media/` from the CC BY
models credited in `assets/media/CREDITS.md`. The models aren't in this repository; download
them from the links there into a `models/` folder (one folder per model, as Sketchfab's
glTF download unpacks, plus `cd_jewel_case.blend`).

Needs Blender's Python module (`pip install bpy`), NumPy and Pillow.

```sh
# a cartridge: CFG is a name from CONFIGS in jc_tpl.py
CFG=wide MO=render/wide MODELS=models python3 tools/render/jc_tpl.py
python3 tools/render/jc_post.py render/wide assets/media/wide 132000      # target area in px
# Game Boy renders sideways: add 1 to rotate it a quarter turn clockwise
python3 tools/render/jc_post.py render/small assets/media/small 84000 1

# discs
MO=render/disc MODELS=models python3 tools/render/jc_disc.py
python3 tools/render/jc_post_disc.py render/disc assets/media/disc_black 380 '#121216'

make media   # rebuilds src/gs/media.h with the new templates
```

`jc_tpl.py` writes `beauty.png` (the model with a blank white sticker) and `mask.png` (where
the sticker is). The post scripts turn those into `base.png`, `over.png` (the shading and
edges drawn over the cover) and `meta.json` (the label rectangle, or the disc's radii).
