"""Read-only audit of complete API v3 firmware artifacts; never prints secrets."""

import hashlib
import json
from pathlib import Path
import re
import zipfile

from check_repository import ROOT, local_secrets
from build_firmware_release import PROFILES


def verify(root=ROOT):
    root = Path(root)
    folder = root / "dist/firmware"
    manifest = json.loads((folder / "manifest.json").read_text())
    if manifest.get("api_version") != 3 or manifest.get("credential_free") is not True:
        raise ValueError("Expected credential-free API v3 release")
    profiles = {item["profile"]: item for item in manifest["profiles"]}
    if set(profiles) != set(PROFILES):
        raise ValueError("Release must contain exactly all seven board profiles")
    hashes = {}
    for line in (folder / "SHA256SUMS.txt").read_text().splitlines():
        digest, name = line.split("  ", 1)
        path = (folder / name).resolve()
        if not path.is_relative_to(folder.resolve()) or path.suffix != ".bin":
            raise ValueError("Unsafe checksum path")
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError("Binary checksum mismatch: " + name)
        hashes[name] = digest
    if set(hashes) != {p.relative_to(folder).as_posix() for p in folder.rglob("*.bin")}:
        raise ValueError("Checksum inventory differs from binary files")
    secrets = [s.encode() for s in local_secrets(root)]
    api_key = re.compile(rb"(?:gsk_[A-Za-z0-9]{20,}|sk-(?:proj-)?[A-Za-z0-9_-]{24,})")
    private_key = re.compile(
        rb"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----\r?\n[A-Za-z0-9+/=\r\n]{64,}-----END"
    )
    for name in hashes:
        data = (folder / name).read_bytes()
        if (
            any(value in data for value in secrets)
            or api_key.search(data)
            or private_key.search(data)
        ):
            raise ValueError("Credential audit failed: " + name)
    for profile, item in profiles.items():
        directory = folder / profile
        factory = (directory / "factory.bin").read_bytes()
        if hashlib.sha256(factory).hexdigest() != item["factory_sha256"]:
            raise ValueError("Factory checksum mismatch: " + profile)
        plan = json.loads((directory / "flash-plan.json").read_text())
        previous_end = 0
        for image in sorted(plan["images"], key=lambda row: row["offset"]):
            path = (directory / image["file"]).resolve()
            if path.parent != directory.resolve():
                raise ValueError("Unsafe flash-plan path")
            data = path.read_bytes()
            start = image["offset"]
            if start < previous_end or factory[start : start + len(data)] != data:
                raise ValueError("Invalid merged segment: " + profile)
            if factory[previous_end:start] != b"\xff" * (start - previous_end):
                raise ValueError("Unexpected data between flash segments: " + profile)
            previous_end = start + len(data)
        if previous_end != len(factory):
            raise ValueError("Unexpected trailing data: " + profile)
    archive = root / "dist/esp32-mcp-firmware.zip"
    with zipfile.ZipFile(archive) as bundle:
        if bundle.testzip() is not None:
            raise ValueError("Corrupt firmware ZIP")
        for path in folder.rglob("*"):
            if (
                path.is_file()
                and bundle.read("firmware/" + path.relative_to(folder).as_posix())
                != path.read_bytes()
            ):
                raise ValueError("ZIP content differs from audited artifacts")
    return {
        "profiles": len(profiles),
        "binary_images": len(hashes),
        "api_version": 3,
        "checksums": "passed",
        "merged_segments": "passed",
        "credentials": "passed",
        "zip": "passed",
    }


if __name__ == "__main__":
    print(json.dumps(verify()))
