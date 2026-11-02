"""PIO post-build hook, enabled only by build_firmware_release.py in a clean copy."""

import json
from pathlib import Path

Import("env")  # noqa: F821 -- supplied by PlatformIO/SCons


def record_images(source, target, env):
    board = env.BoardConfig()
    images = [
        {"offset": int(str(offset), 0), "path": str(Path(env.subst(path)).resolve())}
        for offset, path in env.get("FLASH_EXTRA_IMAGES", [])
    ]
    images.append(
        {
            "offset": int(env.subst("$ESP32_APP_OFFSET"), 0),
            "path": str(Path(env.subst("$BUILD_DIR/${PROGNAME}.bin")).resolve()),
        }
    )
    value = {
        "profile": env.subst("$PIOENV"),
        "chip": board.get("build.mcu"),
        "flash_size": board.get("upload.flash_size"),
        "images": images,
    }
    Path(env.subst("$BUILD_DIR/flash_manifest.json")).write_text(
        json.dumps(value, indent=2), encoding="utf-8"
    )


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", record_images)  # noqa: F821
