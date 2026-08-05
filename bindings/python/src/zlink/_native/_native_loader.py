# SPDX-License-Identifier: MPL-2.0

import ctypes
import os
import pathlib
import platform
import sys


SUPPORTED_PLATFORM = "linux-x86_64"


def _require_supported_platform():
    machine = platform.machine().lower()
    if not sys.platform.startswith("linux") or machine not in {"x86_64", "amd64"}:
        raise OSError(
            "zlink Python Core 11 wheels currently support Linux x86_64 only"
        )


def load_native_library(bind=None):
    _require_supported_platform()
    candidates = []
    path = os.environ.get("ZLINK_LIBRARY_PATH")
    if path:
        candidates.append(path)
    else:
        for candidate in (
            _find_bundled_library(),
            _find_prefix_library(),
        ):
            if candidate and candidate not in candidates:
                candidates.append(candidate)
    if not candidates:
        raise OSError("zlink native library not found")

    last_error = None
    for candidate in candidates:
        try:
            lib = ctypes.CDLL(candidate)
            if bind is not None:
                bind(lib)
            return lib
        except (AttributeError, OSError) as exc:
            last_error = exc
    raise OSError("zlink native library not found or incompatible") from last_error


def _find_bundled_library():
    _require_supported_platform()
    base = pathlib.Path(__file__).resolve().parent.parent
    candidate = base / "native" / SUPPORTED_PLATFORM / "libzlink.so"
    if candidate.exists():
        return str(candidate)
    return None


def _find_prefix_library():
    prefix_value = os.environ.get("ZLINK_CORE_PREFIX")
    if not prefix_value:
        return None

    prefix = pathlib.Path(prefix_value).expanduser()
    candidate = prefix / "lib" / "libzlink.so"
    if candidate.exists():
        return str(candidate)
    return None
