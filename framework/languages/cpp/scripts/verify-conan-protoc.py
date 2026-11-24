"""Verify the protoc command generated for the Framework schema."""

import json
from pathlib import Path
import re
import subprocess
import sys


def main():
    build_dir, provider_file = map(Path, sys.argv[1:])
    provider = json.loads(provider_file.read_text(encoding="utf-8"))
    expected_protoc = Path(provider["protoc"]).resolve(strict=True)
    command = subprocess.check_output(
        ["ninja", "-C", build_dir, "-t", "commands", "-s", "channel-header-v1.pb.h"],
        text=True,
    )
    invocation = re.search(r'&&\s+(?:"([^"]+)"|(\S+))\s+--cpp_out(?=[=\s])', command)
    if invocation is None:
        raise SystemExit(f"Cannot locate protoc in Ninja command: {command.strip()}")
    protoc = Path(invocation[1] or invocation[2])
    if not protoc.is_absolute() or protoc.resolve() != expected_protoc:
        raise SystemExit(f"Ninja uses {protoc}; Conan selected {expected_protoc}")
    version = subprocess.check_output([protoc, "--version"], text=True).strip()
    expected_version = "libprotoc " + provider["version"].split(".", 1)[1]
    if version != expected_version:
        raise SystemExit(f"{protoc} reports {version}; expected {expected_version}")
    print(f"Verified Ninja protoc: {expected_protoc} ({version})")


if __name__ == "__main__":
    main()
