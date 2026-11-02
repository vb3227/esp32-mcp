"""Build public credential-free app/factory .bin images in an isolated source copy.

Never builds a release from the private working header and never flashes hardware.
"""

import argparse
import configparser
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

from check_repository import check, local_secrets

ROOT = Path(__file__).resolve().parents[1]
PROFILES = ("esp32dev", "esp32s3", "esp32c3", "esp32c6", "esp32s2", "esp32wrover", "k10")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pio", default=shutil.which("pio") or shutil.which("platformio"))
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--profiles", nargs="+", choices=PROFILES, default=list(PROFILES))
    parser.add_argument(
        "--artifact-dir",
        type=Path,
        help="Assemble previously built GitHub matrix artifacts (firmware-PROFILE folders)",
    )
    args = parser.parse_args()
    if (not args.pio and not args.artifact_dir) or not 1 <= args.jobs <= 8:
        parser.error("Install PlatformIO and choose 1..8 compiler jobs")
    files, issues = check(ROOT)
    if issues:
        raise SystemExit("Source publication preflight failed; no firmware release built")
    workspace = ROOT / ".esp_mcp/release-workspace"
    workspace.mkdir(parents=True, exist_ok=True)
    # Refresh only allowlisted source files. Runtime state/private headers never enter.
    for path in files:
        if path == ROOT / "platformio.ini":
            continue
        destination = workspace / path.relative_to(ROOT)
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.exists() or destination.read_bytes() != path.read_bytes():
            shutil.copyfile(path, destination)
    public_header = workspace / "include/board_config.example.h"
    target_header = workspace / "include/board_config.h"
    if not target_header.exists() or target_header.read_bytes() != public_header.read_bytes():
        shutil.copyfile(public_header, target_header)
    settings = configparser.ConfigParser(interpolation=None)
    settings.read(ROOT / "platformio.ini", encoding="utf-8")
    for section in settings.sections():
        if section not in ("generic", "env:k10") and not section.startswith("env:"):
            continue
        if section in ("generic", "env:k10"):
            settings[section]["extra_scripts"] = "post:scripts/platformio_release.py"
        # Strip workstation prefixes from __FILE__ strings in public binaries.
        if "build_flags" in settings[section]:
            settings[section]["build_flags"] += (
                "\n-ffile-prefix-map=" + Path.home().as_posix() + "=/build"
            )
    stream = io.StringIO()
    settings.write(stream)
    config_path = workspace / "platformio.ini"
    rendered = stream.getvalue()
    if not config_path.exists() or config_path.read_text(encoding="utf-8") != rendered:
        config_path.write_text(rendered, encoding="utf-8")
    output = ROOT / "dist/firmware"
    output.mkdir(parents=True, exist_ok=True)
    manifest = {"credential_free": True, "api_version": 3, "profiles": []}
    secrets = [value.encode() for value in local_secrets(ROOT)]
    for profile in args.profiles:
        if args.artifact_dir:
            artifact = (args.artifact_dir / ("firmware-" + profile)).resolve()
            plan = json.loads((artifact / "flash-plan.json").read_text(encoding="utf-8"))
            if plan.get("profile") != profile:
                raise SystemExit("Artifact profile mismatch")
            images = []
            for row in plan["images"]:
                source = (artifact / row["file"]).resolve()
                if source.parent != artifact or source.suffix != ".bin":
                    raise SystemExit("Unsafe firmware artifact path")
                images.append({"path": str(source), "offset": row["offset"]})
            flash = {"chip": plan["chip"], "flash_size": plan["flash_size"], "images": images}
        else:
            command = [args.pio, "run", "-e", profile, "-j", str(args.jobs)]
            print("Building public profile:", profile, flush=True)
            subprocess.run(command, cwd=workspace, check=True)
            build = workspace / ".pio/build" / profile
            flash = json.loads((build / "flash_manifest.json").read_text(encoding="utf-8"))
        destination = output / profile
        destination.mkdir(exist_ok=True)
        entries = []
        for index, item in enumerate(flash["images"]):
            source = Path(item["path"])
            data = source.read_bytes()
            if any(secret in data for secret in secrets):
                raise SystemExit("Local credential detected; firmware release blocked")
            name = (
                source.name
                if args.artifact_dir
                else (
                    "firmware.bin"
                    if source.name == "firmware.bin"
                    else f"image-{index}-{source.name}"
                )
            )
            (destination / name).write_bytes(data)
            entries.append({"offset": item["offset"], "file": name})
        end = max(row["offset"] + (destination / row["file"]).stat().st_size for row in entries)
        if end > 16 * 1024 * 1024:
            raise SystemExit("Unexpected flash-image size")
        # Factory image starts at 0. Preserve each vendor-generated boot header.
        merged = bytearray(b"\xff") * end
        previous_end = 0
        for item in sorted(entries, key=lambda row: row["offset"]):
            data = (destination / item["file"]).read_bytes()
            if item["offset"] < previous_end:
                raise SystemExit("Overlapping flash segments")
            merged[item["offset"] : item["offset"] + len(data)] = data
            previous_end = item["offset"] + len(data)
        factory = destination / "factory.bin"
        factory.write_bytes(merged)
        plan = {
            "profile": profile,
            "chip": flash["chip"],
            "flash_size": flash.get("flash_size"),
            "factory_offset": 0,
            "images": entries,
        }
        (destination / "flash-plan.json").write_text(json.dumps(plan, indent=2), encoding="utf-8")
        manifest["profiles"].append(
            {
                "profile": profile,
                "chip": flash["chip"],
                "factory_sha256": hashlib.sha256(merged).hexdigest(),
            }
        )
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    for source, name in (
        (ROOT / "LICENSE", "LICENSE"),
        (ROOT / "THIRD_PARTY_NOTICES.md", "THIRD_PARTY_NOTICES.md"),
        (ROOT / "docs/INSTALL_BIN_RU.md", "README.md"),
        (ROOT / "scripts/configure_device.py", "configure_device.py"),
        (ROOT / "config/mcp.example.toml", "mcp.example.toml"),
        (ROOT / "config/k10.mcp.example.toml", "k10.mcp.example.toml"),
    ):
        shutil.copyfile(source, output / name)
    lua = (ROOT / "lib/lua/src/lua.h").read_text(encoding="utf-8")
    start = lua.index("Copyright (C) 1994-2026 Lua.org")
    end = lua.index(
        "******************************************************************************/", start
    )
    (output / "THIRD_PARTY_NOTICES.txt").write_text(
        "ESP32 MCP firmware third-party notices\n\nLua 5.4.9 (MIT):\n"
        + lua[start:end]
        + "\nArduino-ESP32 / UNIHIKER vendor Arduino framework: upstream licensing applies.\n"
        + "https://github.com/espressif/arduino-esp32\nhttps://github.com/DFRobot/platform-unihiker\n"
        + "ESP-IDF: https://github.com/espressif/esp-idf (Apache-2.0)\n"
        + "LVGL (K10): https://github.com/lvgl/lvgl (MIT)\n"
        + "NimBLE-Arduino 2.5.2: https://github.com/h2zero/NimBLE-Arduino (Apache-2.0)\n"
        + "ASR model assets are not included. Source/build scripts are distributed separately.\n",
        encoding="utf-8",
    )
    hashes = []
    for path in sorted(output.rglob("*.bin")):
        data = path.read_bytes()
        if any(secret in data for secret in secrets):
            raise SystemExit("Credential audit failed")
        hashes.append(hashlib.sha256(data).hexdigest() + "  " + path.relative_to(output).as_posix())
    (output / "SHA256SUMS.txt").write_text("\n".join(hashes) + "\n", encoding="utf-8")
    archive = ROOT / "dist/esp32-mcp-firmware.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
        for path in sorted(output.rglob("*")):
            if path.is_file():
                bundle.write(path, "firmware/" + path.relative_to(output).as_posix())
    archive.with_name(archive.name + ".sha256").write_text(
        hashlib.sha256(archive.read_bytes()).hexdigest() + "  " + archive.name + "\n",
        encoding="utf-8",
    )
    print("Created credential-free firmware bundle:", archive.name)


if __name__ == "__main__":
    main()
