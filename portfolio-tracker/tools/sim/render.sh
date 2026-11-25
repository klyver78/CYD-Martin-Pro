#!/bin/bash
# Run the real portfolio + UI code on a PC against the recorded API responses in fixtures/,
# print the computed numbers and write docs/*.png. Needs g++, python3 + Pillow.
set -e
cd "$(dirname "$0")"
rm -rf out ../../docs/*.png; mkdir -p out ../../docs
# 1. compile-check the Arduino-only sources against the mock headers (they are not linked;
#    this only proves they still compile, since the cloud sandbox has no ESP32 toolchain).
g++ -std=gnu++17 -fsyntax-only -Wall -Wextra -Wno-unused-parameter -Imock -I../../src \
    ../../src/net.cpp ../../src/main.cpp

# 2. build and run the real portfolio + UI code against the recorded API responses.
g++ -std=gnu++17 -O2 -Wall -Wextra -Wno-unused-parameter -Imock -I../../src -o out/sim sim.cpp
./out/sim
python3 - <<'PY'
import glob, os
from PIL import Image
for p in sorted(glob.glob('out/*.ppm')):
    Image.open(p).resize((640, 480), Image.NEAREST).save('../../docs/' + os.path.basename(p)[:-4] + '.png', optimize=True)
print('Wrote docs/*.png')
PY
