#!/usr/bin/env python3
"""Run an end-to-end hardware smoke test against the bundled FlashGBX CLI."""

import argparse
import glob
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
from datetime import datetime
from pathlib import Path


DMG_LOGO = bytes([
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B,
    0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
    0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
    0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
    0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC,
    0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
])


def eprint(message):
    print(message, file=sys.stderr)


def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def file_info(path):
    if not path.exists():
        return {"exists": False}
    return {
        "exists": True,
        "size": path.stat().st_size,
        "sha256": sha256(path),
    }


def sanitize_stem(text, fallback):
    text = (text or "").strip()
    if not text:
        text = fallback
    text = re.sub(r"[^A-Za-z0-9._+-]+", "_", text)
    text = text.strip("._-")
    return text or fallback


def infer_port():
    patterns = [
        "/dev/cu.usbserial*",
        "/dev/cu.usbmodem*",
        "/dev/tty.usbserial*",
        "/dev/tty.usbmodem*",
    ]
    ports = []
    for pattern in patterns:
        ports.extend(glob.glob(pattern))
    ports = sorted(dict.fromkeys(ports))
    if not ports:
        return None
    cu_ports = [port for port in ports if Path(port).name.startswith("cu.")]
    return (cu_ports or ports)[0]


def cli_names():
    names = ["flashgbx-cli"]
    if os.name == "nt":
        names.insert(0, "flashgbx-cli.exe")
    return names


def cli_candidates(app):
    if app:
        app = Path(app)
        roots = [
            app / "Contents/Resources",
            app,
            app.parent,
        ]
    else:
        roots = [
            Path("dist/mgba-macos/Applications/mGBA.app/Contents/Resources"),
            Path("build/qt"),
            Path("build/install"),
            Path("package"),
            Path("."),
        ]
    candidates = []
    for root in roots:
        for name in cli_names():
            candidates.append(root / "FlashGBX/flashgbx-cli" / name)
    return candidates


def resolve_cli(args):
    if args.cli:
        candidates = [Path(args.cli)]
    else:
        candidates = cli_candidates(args.app)
    for cli in candidates:
        if cli.exists() and os.access(str(cli), os.X_OK):
            return cli
    raise SystemExit("Bundled flashgbx-cli was not found. Checked: {}".format(
        ", ".join(str(candidate) for candidate in candidates)))


def flashgbx_options(args, mode):
    options = ["--cfgdir", "subdir"]
    if args.port:
        options.extend(["--device-port", args.port])
    if args.flashcart_type:
        options.extend(["--flashcart-type", args.flashcart_type])
    if mode == "dmg":
        options.extend(["--dmg-savetype", args.dmg_savetype])
        options.extend(["--dmg-mbc", args.dmg_mbc])
    else:
        options.extend(["--agb-savetype", args.agb_savetype])
    return options


def run_flashgbx(cli, args, mode, action, path, outdir, label):
    command = [
        str(cli),
        "--cli",
        "--mode",
        mode,
        "--action",
        action,
        "--overwrite",
    ]
    command.extend(flashgbx_options(args, mode))
    command.append(str(path))

    log_path = outdir / "{}.log".format(label)
    eprint("$ {}".format(shlex.join(command)))
    proc = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log_path.write_text(proc.stdout, encoding="utf-8", errors="replace")
    if proc.stdout:
        print(proc.stdout, end="" if proc.stdout.endswith("\n") else "\n")
    return {
        "label": label,
        "action": action,
        "mode": mode,
        "path": str(path),
        "command": command,
        "exit_code": proc.returncode,
        "log": str(log_path),
        "output": proc.stdout,
    }


def parse_info(output):
    info = {}
    for raw_line in output.splitlines():
        line = raw_line.strip()
        if not line or ":" not in line:
            continue
        key, value = line.split(":", 1)
        key = key.strip()
        value = value.strip()
        if key:
            info[key] = value
    return info


def info_looks_valid(result):
    if result["exit_code"] != 0:
        return False
    output = result["output"]
    return "Cartridge Information:" in output and ("Game Title:" in output or "Game Name:" in output)


def detect_mode(cli, args, outdir):
    if args.mode != "auto":
        result = run_flashgbx(cli, args, args.mode, "info", "auto", outdir, "info-{}".format(args.mode))
        if not info_looks_valid(result):
            raise SystemExit("FlashGBX info failed for mode {}. See {}".format(args.mode, result["log"]))
        return args.mode, result

    attempts = []
    for mode in ("dmg", "agb"):
        result = run_flashgbx(cli, args, mode, "info", "auto", outdir, "info-{}".format(mode))
        attempts.append(result)
        if info_looks_valid(result):
            return mode, result

    logs = ", ".join(result["log"] for result in attempts)
    raise SystemExit("Could not detect a readable cartridge mode. See {}".format(logs))


def validate_dmg_rom(path):
    data = path.read_bytes()
    if len(data) < 0x150:
        return False, "DMG ROM is too small"
    if len(set(data[: min(len(data), 4096)])) <= 1:
        return False, "DMG ROM looks blank"
    if data[0x104:0x134] != DMG_LOGO:
        return False, "DMG Nintendo logo is invalid"
    checksum = 0
    for value in data[0x134:0x14D]:
        checksum = (checksum - value - 1) & 0xFF
    if checksum != data[0x14D]:
        return False, "DMG header checksum mismatch"
    return True, "DMG header is valid"


def validate_agb_rom(path):
    data = path.read_bytes()
    if len(data) < 0xC0:
        return False, "AGB ROM is too small"
    if len(set(data[: min(len(data), 4096)])) <= 1:
        return False, "AGB ROM looks blank"
    checksum = 0
    for value in data[0xA0:0xBD]:
        checksum = (checksum - value) & 0xFF
    checksum = (checksum - 0x19) & 0xFF
    if checksum != data[0xBD]:
        return False, "AGB header checksum mismatch"
    return True, "AGB header is valid"


def validate_rom(path, mode):
    if not path.exists():
        return False, "ROM backup file was not created"
    if mode == "dmg":
        return validate_dmg_rom(path)
    return validate_agb_rom(path)


def rtc_warning(info, mode, dmg_mbc):
    mapper = info.get("Mapper Type", "")
    rtc_line = info.get("Real Time Clock", "")
    selected_rtc = mode == "dmg" and dmg_mbc.lower() in {"0x10", "0x110", "0xfe", "0xfd"}
    expected_rtc = selected_rtc or "RTC" in mapper.upper()
    if not expected_rtc:
        return None
    lowered = rtc_line.lower()
    if any(token in lowered for token in ("not available", "invalid", "battery dry")):
        return "RTC data is not stable: {}".format(rtc_line or "missing")
    match = re.search(r"(\d+)\s+days?,", rtc_line, re.IGNORECASE)
    if match and int(match.group(1)) == 0:
        return "RTC looks reset: {}".format(rtc_line)
    if not rtc_line:
        return "RTC line is missing for an RTC cartridge"
    return None


def collect_rtc_statuses(actions):
    statuses = []
    seen = set()
    for action in actions:
        info = parse_info(action.get("output", ""))
        rtc = info.get("Real Time Clock")
        if not rtc:
            continue
        key = (action.get("label"), rtc)
        if key in seen:
            continue
        seen.add(key)
        statuses.append({
            "label": action.get("label"),
            "real_time_clock": rtc,
        })
    return statuses


def collect_rtc_warnings(actions, mode, dmg_mbc):
    warnings = []
    seen = set()
    for action in actions:
        warning = rtc_warning(parse_info(action.get("output", "")), mode, dmg_mbc)
        if not warning:
            continue
        warning = "{}: {}".format(action.get("label"), warning)
        if warning in seen:
            continue
        seen.add(warning)
        warnings.append(warning)
    return warnings


def save_type_has_data(info):
    save_type = info.get("Save Type", "").strip().lower()
    if not save_type:
        return True
    return save_type not in {"none", "no save", "n/a"}


def build_summary(args, cli, outdir):
    mode, info_result = detect_mode(cli, args, outdir)
    info = parse_info(info_result["output"])
    game_title = info.get("Game Title") or info.get("Game Code") or info.get("Game Name")
    stem = sanitize_stem(game_title, "cartridge-{}".format(mode))
    rom_path = outdir / "{}.{}".format(stem, "gb" if mode == "dmg" else "gba")
    save_path = outdir / "{}.sav".format(stem)
    verify_save_path = outdir / "{}.verify.sav".format(stem)
    write_verify_path = outdir / "{}.writeback.sav".format(stem)

    warnings = []
    failures = []
    actions = [info_result]

    if not args.skip_save and save_type_has_data(info):
        save_result = run_flashgbx(cli, args, mode, "backup-save", save_path, outdir, "backup-save")
        actions.append(save_result)
        verify_result = run_flashgbx(cli, args, mode, "backup-save", verify_save_path, outdir, "backup-save-verify")
        actions.append(verify_result)
        if save_result["exit_code"] != 0:
            failures.append("backup-save failed")
        if verify_result["exit_code"] != 0:
            failures.append("backup-save verify pass failed")
        save_info = file_info(save_path)
        verify_info = file_info(verify_save_path)
        if not save_info.get("exists"):
            failures.append("save file was not created")
        elif save_info.get("size", 0) <= 0:
            failures.append("save file is empty")
        if not verify_info.get("exists"):
            failures.append("second save file was not created")
        elif verify_info.get("size", 0) <= 0:
            failures.append("second save file is empty")
        if save_info.get("exists") and verify_info.get("exists") and save_info.get("sha256") != verify_info.get("sha256"):
            failures.append("two consecutive save backups have different hashes")
        if save_path.suffix != ".sav" or save_path.stem != rom_path.stem:
            failures.append("save path is not the same stem as the ROM path with .sav suffix")

        if args.write_save_verify and save_info.get("exists") and save_info.get("size", 0) > 0:
            restore_result = run_flashgbx(cli, args, mode, "restore-save", save_path, outdir, "restore-save")
            actions.append(restore_result)
            writeback_result = run_flashgbx(cli, args, mode, "backup-save", write_verify_path, outdir, "backup-save-writeback")
            actions.append(writeback_result)
            writeback_info = file_info(write_verify_path)
            if restore_result["exit_code"] != 0:
                failures.append("restore-save failed")
            if writeback_result["exit_code"] != 0:
                failures.append("post-restore backup-save failed")
            if writeback_info.get("sha256") != save_info.get("sha256"):
                failures.append("post-restore readback hash does not match")
    elif not args.skip_save:
        warnings.append("Cartridge reports no save data type; save backup was skipped")

    if not args.skip_rom:
        rom_result = run_flashgbx(cli, args, mode, "backup-rom", rom_path, outdir, "backup-rom")
        actions.append(rom_result)
        if rom_result["exit_code"] != 0:
            failures.append("backup-rom failed")
        valid_rom, rom_message = validate_rom(rom_path, mode)
        if not valid_rom:
            failures.append(rom_message)
    else:
        valid_rom, rom_message = None, "ROM backup skipped"

    for warning in collect_rtc_warnings(actions, mode, args.dmg_mbc):
        if warning not in warnings:
            warnings.append(warning)

    summary = {
        "ok": not failures,
        "created_at": datetime.now().isoformat(timespec="seconds"),
        "mode": mode,
        "port": args.port,
        "cli": str(cli),
        "outdir": str(outdir),
        "info": info,
        "warnings": warnings,
        "rtc_statuses": collect_rtc_statuses(actions),
        "failures": failures,
        "rom": {
            "path": str(rom_path),
            "validation": rom_message,
            **file_info(rom_path),
        },
        "save": {
            "path": str(save_path),
            **file_info(save_path),
        },
        "save_verify": {
            "path": str(verify_save_path),
            **file_info(verify_save_path),
        },
        "save_writeback": {
            "path": str(write_verify_path),
            **file_info(write_verify_path),
        },
        "write_save_verify": args.write_save_verify,
        "actions": [
            {
                "label": action["label"],
                "action": action["action"],
                "mode": action["mode"],
                "exit_code": action["exit_code"],
                "log": action["log"],
            }
            for action in actions
        ],
    }
    return summary


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", default=os.environ.get("MGBA_APP"), help="mGBA app, install prefix, or artifact directory containing FlashGBX")
    parser.add_argument("--cli", help="flashgbx-cli path; overrides --app")
    parser.add_argument("--port", default=os.environ.get("FLASHGBX_PORT"), help="serial device path, e.g. /dev/cu.usbserial-210")
    parser.add_argument("--mode", choices=("auto", "dmg", "agb"), default=os.environ.get("FLASHGBX_MODE", "auto"))
    parser.add_argument("--dmg-savetype", default=os.environ.get("FLASHGBX_DMG_SAVETYPE", "auto"))
    parser.add_argument("--dmg-mbc", default=os.environ.get("FLASHGBX_DMG_MBC", "auto"))
    parser.add_argument("--agb-savetype", default=os.environ.get("FLASHGBX_AGB_SAVETYPE", "auto"))
    parser.add_argument("--flashcart-type", default=os.environ.get("FLASHGBX_FLASHCART_TYPE", "autodetect"))
    parser.add_argument("--outdir", default=os.environ.get("FLASHGBX_TEST_DIR"))
    parser.add_argument("--skip-rom", action="store_true", help="skip ROM backup/header validation")
    parser.add_argument("--skip-save", action="store_true", help="skip save backup/hash validation")
    parser.add_argument("--write-save-verify", action="store_true", help="restore the first save backup, then read it back and compare hashes")
    parser.add_argument("--fail-on-warning", action="store_true", help="return a non-zero exit code when warnings are present")
    return parser.parse_args(argv)


def main(argv):
    args = parse_args(argv)
    cli = resolve_cli(args)
    if not args.port:
        args.port = infer_port()
        if args.port:
            eprint("Using detected cartridge reader port: {}".format(args.port))

    if args.outdir:
        outdir = Path(args.outdir)
    else:
        outdir = Path("dist/cartridge-test") / datetime.now().strftime("%Y%m%d-%H%M%S")
    outdir.mkdir(parents=True, exist_ok=True)

    summary = build_summary(args, cli, outdir)
    summary_path = outdir / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    print("")
    print("Cartridge test summary")
    print("  mode: {}".format(summary["mode"]))
    print("  port: {}".format(summary["port"] or "auto"))
    print("  outdir: {}".format(summary["outdir"]))
    print("  summary: {}".format(summary_path))
    if summary["rom"].get("exists"):
        print("  rom: {} bytes sha256={}".format(summary["rom"]["size"], summary["rom"]["sha256"]))
    if summary["save"].get("exists"):
        print("  save: {} bytes sha256={}".format(summary["save"]["size"], summary["save"]["sha256"]))
    if summary["save_verify"].get("exists"):
        match_text = "match" if summary["save"].get("sha256") == summary["save_verify"].get("sha256") else "mismatch"
        print("  save verify: {} bytes sha256={} ({})".format(
            summary["save_verify"]["size"],
            summary["save_verify"]["sha256"],
            match_text,
        ))
    if summary["save_writeback"].get("exists"):
        match_text = "match" if summary["save"].get("sha256") == summary["save_writeback"].get("sha256") else "mismatch"
        print("  save writeback: {} bytes sha256={} ({})".format(
            summary["save_writeback"]["size"],
            summary["save_writeback"]["sha256"],
            match_text,
        ))
    if summary.get("rtc_statuses"):
        print("  rtc statuses:")
        for status in summary["rtc_statuses"]:
            print("    - {}: {}".format(status["label"], status["real_time_clock"]))
    if summary["warnings"]:
        print("  warnings:")
        for warning in summary["warnings"]:
            print("    - {}".format(warning))
    if summary["failures"]:
        print("  failures:")
        for failure in summary["failures"]:
            print("    - {}".format(failure))
        return 2
    if args.fail_on_warning and summary["warnings"]:
        return 1
    print("  result: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
