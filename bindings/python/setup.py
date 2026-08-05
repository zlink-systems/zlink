# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import os
import platform
import re
import sys
from pathlib import Path
from setuptools import Extension, setup


PACKAGE_ROOT = Path(__file__).resolve().parent
SUPPORTED_PLATFORM = "linux-x86_64"


def _supported_platform() -> str:
    machine = platform.machine().lower()
    if not sys.platform.startswith("linux") or machine not in {"x86_64", "amd64"}:
        raise RuntimeError(
            "zlink Python Core 11 wheels currently support Linux x86_64 only"
        )
    return SUPPORTED_PLATFORM


TARGET_PLATFORM = _supported_platform()


def _core_prefix() -> Path:
    value = os.environ.get("ZLINK_CORE_PREFIX")
    if not value:
        raise RuntimeError(
            "ZLINK_CORE_PREFIX is required; build against an explicit Core "
            "installation prefix rather than the repository build directory"
        )
    prefix = Path(value).expanduser().resolve()
    include_dir = prefix / "include"
    library_dir = prefix / ("bin" if sys.platform == "win32" else "lib")
    if not (include_dir / "zlink.h").is_file():
        raise RuntimeError(f"Core headers are missing from {include_dir}")
    if not any(
        candidate.name.startswith(("libzlink", "zlink"))
        for candidate in library_dir.glob("*")
    ):
        raise RuntimeError(f"Core library is missing from {library_dir}")
    return prefix


CORE_PREFIX = _core_prefix()
CORE_INCLUDE = CORE_PREFIX / "include"
CORE_LIBRARY = CORE_PREFIX / ("bin" if sys.platform == "win32" else "lib")


def _runtime_library_dirs() -> list[str]:
    # The extension is installed under zlink/_native and packaged libzlink
    # lives under zlink/native/<platform>. Keep runtime lookup package-relative
    # so built wheels do not embed source-tree paths.
    _supported_platform()
    return [f"$ORIGIN/../native/{SUPPORTED_PLATFORM}"]


def _compile_args() -> list[str]:
    return ["-O3", "-pthread"]


def _native_extension(name: str, source: str) -> Extension:
    return Extension(
        name,
        [source],
        include_dirs=[str(CORE_INCLUDE)],
        library_dirs=[str(CORE_LIBRARY)],
        libraries=["zlink"],
        runtime_library_dirs=_runtime_library_dirs(),
        extra_compile_args=_compile_args(),
        extra_link_args=["-pthread"],
    )


def _core_version() -> tuple[int, int, int]:
    header = (CORE_INCLUDE / "zlink.h").read_text(encoding="utf-8")
    values = []
    for name in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(rf"^#define ZLINK_VERSION_{name} (\d+)$", header, re.MULTILINE)
        if match is None:
            raise RuntimeError(f"Core header is missing ZLINK_VERSION_{name}")
        values.append(int(match.group(1)))
    return tuple(values)


def _native_package_data() -> dict[str, list[str]]:
    platform_dir = TARGET_PLATFORM
    payload_dir = PACKAGE_ROOT / "src" / "zlink" / "native" / platform_dir
    major, minor, patch = _core_version()
    version = f"{major}.{minor}.{patch}"
    expected_names = {
        "libzlink.so",
        f"libzlink.so.{major}",
        f"libzlink.so.{version}",
    }
    if not (payload_dir / f"libzlink.so.{version}").is_file():
        raise RuntimeError(f"Core runtime payload is missing from {payload_dir}")
    payload_names = {
        path.name
        for path in payload_dir.iterdir()
        if path.is_file() or path.is_symlink()
    }
    unexpected = sorted(
        name for name in payload_names if name.startswith("libzlink") and name not in expected_names
    )
    if unexpected:
        raise RuntimeError(
            "Unsupported or stale Core payload remains in "
            f"{payload_dir}: {', '.join(unexpected)}"
        )
    if not (payload_dir / "libzlink.so").is_symlink():
        raise RuntimeError(f"Core runtime loader link is missing from {payload_dir}")
    return {
        "zlink": [
            "py.typed",
            f"native/{platform_dir}/libzlink.so",
            f"native/{platform_dir}/libzlink.so.{major}",
            f"native/{platform_dir}/libzlink.so.{version}",
        ]
    }


setup(
    package_data=_native_package_data(),
    ext_modules=[
        _native_extension(
            "zlink._native._zlink_native",
            "src/zlink/_native/_zlink_native.c",
        ),
    ]
)
