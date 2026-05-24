#!/usr/bin/env python3
"""Build a standalone FlashGBX CLI runtime for embedding into mGBA."""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


FLASHGBX_DEPENDENCIES = {
    "serial": "pyserial>=3.5",
    "PIL": "Pillow",
    "requests": "requests",
    "dateutil": "python-dateutil",
    "packaging": "packaging",
}


def run(command, cwd=None):
    print("$ {}".format(" ".join(str(part) for part in command)))
    subprocess.run(command, cwd=cwd, check=True)


def module_available(python, module):
    return subprocess.run(
        [python, "-c", "import {}".format(module)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    ).returncode == 0


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build", help="CMake build directory that will receive flashgbx-standalone")
    parser.add_argument("--flashgbx-dir", default="extern/FlashGBX", help="FlashGBX submodule checkout")
    parser.add_argument("--python", default=sys.executable, help="Python executable used for PyInstaller")
    parser.add_argument("--skip-install", action="store_true", help="do not install FlashGBX/PyInstaller dependencies first")
    return parser.parse_args(argv)


def main(argv):
    args = parse_args(argv)
    root = Path.cwd()
    flashgbx_dir = (root / args.flashgbx_dir).resolve()
    build_dir = (root / args.build_dir).resolve()
    dist_dir = build_dir / "flashgbx-standalone"
    work_dir = build_dir / "pyinstaller-work"
    spec_dir = build_dir / "pyinstaller-spec"
    os.environ.setdefault("PIP_CACHE_DIR", str(build_dir / "pip-cache"))
    os.environ.setdefault("PYINSTALLER_CONFIG_DIR", str(build_dir / "pyinstaller-cache"))

    runner = root / "tools/flashgbx-bundled-runner.py"
    resources = flashgbx_dir / "FlashGBX/res"
    if not (flashgbx_dir / "FlashGBX/FlashGBX.py").exists():
        raise SystemExit("FlashGBX submodule is missing: {}".format(flashgbx_dir))
    if not runner.exists():
        raise SystemExit("Bundled runner is missing: {}".format(runner))

    if not args.skip_install and not module_available(args.python, "PyInstaller"):
        run([args.python, "-m", "pip", "install", "pyinstaller"])
    if not args.skip_install:
        missing = [package for module, package in FLASHGBX_DEPENDENCIES.items() if not module_available(args.python, module)]
        if missing:
            run([args.python, "-m", "pip", "install", *missing])

    if dist_dir.exists():
        shutil.rmtree(dist_dir)
    work_dir.mkdir(parents=True, exist_ok=True)
    spec_dir.mkdir(parents=True, exist_ok=True)

    add_data_separator = ";" if os.name == "nt" else ":"
    command = [
        args.python,
        "-m",
        "PyInstaller",
        "--noconfirm",
        "--clean",
        "--onedir",
        "--console",
        "--name",
        "flashgbx-cli",
        "--distpath",
        str(dist_dir),
        "--workpath",
        str(work_dir),
        "--specpath",
        str(spec_dir),
        "--paths",
        str(flashgbx_dir),
    ]
    if resources.exists():
        command.extend(["--add-data", "{}{}res".format(resources, add_data_separator)])
    command.append(str(runner))
    run(command)

    executable = dist_dir / "flashgbx-cli" / ("flashgbx-cli.exe" if os.name == "nt" else "flashgbx-cli")
    if not executable.exists():
        raise SystemExit("PyInstaller did not create {}".format(executable))
    print("Built {}".format(executable))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
