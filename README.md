# CYD-Martin-Pro

Small experiments on the ESP32 **Cheap Yellow Display** (ESP32-2432S028R). Each experiment
lives in its own folder, has its own web flasher on GitHub Pages and can contain several ROMs.

👉 **All flashers: <https://klyver78.github.io/CYD-Martin-Pro/>**

| Experiment | What it tests |
|---|---|
| [animation-experiment](animation-experiment/) | Realistic frame rates with TFT_eSPI, LovyanGFX and LVGL: raw throughput, sprites, full-frame effects, UI animation. Switch ROMs over the air from the device. |
| [space-game](space-game/) | *Star Ring*: a space-game prototype. Orbit a ring, shoot two kinds of shielded enemies flying out of the starfield, catch Spread/Laser/Grenade power-ups. TFT_eSPI 80 MHz. |
| [portfolio-tracker](portfolio-tracker/) | Tracks a portfolio of securities: total value, a two-week value chart and a scrollable list of holdings with today's change. Holdings come from a `portfolio.json` on Pages; prices from the Yahoo Finance API. |

## How the pipeline works

`.github/workflows/pages.yml` runs `tools/build_site.py` on every push to `main`. The script
finds every top-level folder with a `roms.json` + `platformio.ini` and builds each ROM listed
in it (`pio run -e <env>`). It then puts together `_site/`:

```
index.html, experiments.json              overview page
<experiment>/index.html                   web flasher (from <experiment>/web/index.html)
<experiment>/roms.json                    ROM list (read by the device for OTA switching)
<experiment>/manifest-<rom>.json          esp-web-tools manifest
<experiment>/roms/<rom>/merged.bin        factory image (web flasher)
<experiment>/roms/<rom>/firmware.bin      app image (OTA)
```

Pull requests build too, but they don't deploy.

> GitHub Pages must be set to **Source: GitHub Actions** (Settings → Pages).

## Adding an experiment

Create a folder with a PlatformIO project, a `VERSION` file, a `roms.json` (see
`animation-experiment/roms.json`) and `web/index.html`. The pipeline picks it up
automatically. A `"site_files"` list in `roms.json` publishes extra files next to the flasher —
that is how `portfolio-tracker` serves the `portfolio.json` its firmware reads at boot.
