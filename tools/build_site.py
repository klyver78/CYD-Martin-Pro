#!/usr/bin/env python3
"""Build every experiment in the repo and assemble the GitHub Pages site in _site/.

An experiment is any top-level folder with a roms.json + platformio.ini. For each ROM listed
in roms.json the PlatformIO env is built, and the site gets:

  <experiment>/index.html                  web flasher (from <experiment>/web/index.html)
  <experiment>/roms.json                   ROM list read by the device's OTA ROM switcher
  <experiment>/manifest-<rom>.json         esp-web-tools manifest per ROM
  <experiment>/roms/<rom>/merged.bin       factory image for the web flasher (offset 0)
  <experiment>/roms/<rom>/firmware.bin     app-only image for OTA
  index.html + experiments.json            overview of all experiments

Usage:  python tools/build_site.py [--only <experiment>] [--skip-build]
"""
import argparse, json, os, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SITE = ROOT / "_site"


def run(cmd, cwd=None):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, check=True)


def boot_app0():
    base = Path.home() / ".platformio" / "packages" / "framework-arduinoespressif32"
    hits = list(base.rglob("boot_app0.bin"))
    if not hits:
        sys.exit("boot_app0.bin not found - was PlatformIO's Arduino framework installed?")
    return hits[0]


def build_experiment(exp: Path, skip_build: bool):
    meta = json.loads((exp / "roms.json").read_text())
    version = (exp / "VERSION").read_text().strip()
    out = SITE / exp.name
    (out / "roms").mkdir(parents=True, exist_ok=True)

    # version header the firmware reads (FW_VERSION_SEM)
    (exp / "src" / "build_version.h").write_text(f'#define FW_VERSION_SEM "{version}"\n')

    site_roms = []
    for rom in meta["roms"]:
        rid, env = rom["id"], rom["env"]
        if not skip_build:
            run(["pio", "run", "-e", env], cwd=exp)
        build = exp / ".pio" / "build" / env
        rdir = out / "roms" / rid
        rdir.mkdir(parents=True, exist_ok=True)
        shutil.copy(build / "firmware.bin", rdir / "firmware.bin")
        run([sys.executable, "-m", "esptool", "--chip", "esp32", "merge_bin",
             "-o", rdir / "merged.bin",
             "--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "4MB",
             "0x1000", build / "bootloader.bin",
             "0x8000", build / "partitions.bin",
             "0xe000", boot_app0(),
             "0x10000", build / "firmware.bin"])
        manifest = {
            "name": f'{meta["title"]} - {rom["name"]}',
            "version": version,
            "new_install_prompt_erase": True,
            "builds": [{"chipFamily": "ESP32",
                        "parts": [{"path": f"roms/{rid}/merged.bin", "offset": 0}]}],
        }
        (out / f"manifest-{rid}.json").write_text(json.dumps(manifest, indent=2))
        site_roms.append({
            "id": rid, "name": rom["name"], "desc": rom["desc"],
            "bin": f"roms/{rid}/firmware.bin",
            "size": (rdir / "firmware.bin").stat().st_size,
        })

    (out / "roms.json").write_text(json.dumps(
        {"experiment": meta["id"], "title": meta["title"], "version": version, "roms": site_roms}, indent=2))
    shutil.copy(exp / "web" / "index.html", out / "index.html")
    if (exp / "docs").is_dir():
        shutil.copytree(exp / "docs", out / "docs", dirs_exist_ok=True)
    return {"id": meta["id"], "path": exp.name, "title": meta["title"], "summary": meta.get("summary", ""),
            "version": version, "roms": len(site_roms)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only")
    ap.add_argument("--skip-build", action="store_true")
    a = ap.parse_args()

    if SITE.exists():
        shutil.rmtree(SITE)
    SITE.mkdir()
    exps = sorted(p for p in ROOT.iterdir()
                  if p.is_dir() and (p / "roms.json").exists() and (p / "platformio.ini").exists())
    if a.only:
        exps = [p for p in exps if p.name == a.only]
    index = [build_experiment(e, a.skip_build) for e in exps]
    (SITE / "experiments.json").write_text(json.dumps(index, indent=2))
    shutil.copy(ROOT / "site" / "index.html", SITE / "index.html")
    (SITE / ".nojekyll").write_text("")
    for f in sorted(SITE.rglob("*")):
        if f.is_file():
            print(f"{f.relative_to(SITE)}  {f.stat().st_size}")


if __name__ == "__main__":
    main()
