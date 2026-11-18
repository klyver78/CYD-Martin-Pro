#!/bin/bash
# Render the menu and every gfx scene to PNG on a PC (no hardware needed).
# Compiles the real shell + scene code against small mock headers (tools/sim/mock), with
# ArduinoJson, then writes docs/*.png. Needs: g++, git, python3 + Pillow.
set -e
cd "$(dirname "$0")"
LIB=${LIB:-/tmp/cydsim-libs}
mkdir -p "$LIB" out ../../docs
[ -d "$LIB/ArduinoJson" ] || git clone -q --depth 1 -b v7.4.2 https://github.com/bblanchon/ArduinoJson "$LIB/ArduinoJson"
g++ -std=gnu++17 -O1 -w -Imock -I"$LIB/ArduinoJson/src" -o out/sim sim.cpp
./out/sim
python3 - <<'PY'
import glob, os
from PIL import Image
for p in sorted(glob.glob('out/*.ppm')):
    im = Image.open(p).resize((640, 480), Image.NEAREST)
    im.save('../../docs/' + os.path.basename(p)[:-4] + '.png', optimize=True)
print('Wrote docs/*.png')
PY
