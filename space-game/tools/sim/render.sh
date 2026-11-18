#!/bin/bash
# Render title / gameplay / game-over screenshots on a PC -> docs/*.png. Needs g++, python3 + Pillow.
set -e
cd "$(dirname "$0")"
mkdir -p out ../../docs
g++ -std=gnu++17 -O2 -w -Imock -I../../src -o out/sim sim.cpp
./out/sim
python3 - <<'PY'
import glob, os
from PIL import Image
for p in sorted(glob.glob('out/*.ppm')):
    Image.open(p).resize((640, 480), Image.NEAREST).save('../../docs/' + os.path.basename(p)[:-4] + '.png', optimize=True)
print('Wrote docs/*.png')
PY
