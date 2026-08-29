# SPDX-License-Identifier: MPL-2.0

import ctypes
import os
import pathlib
import platform
import sys


def _platform_key():
    machine = platform.machine().lower()
    if sys.platform.startswith("linux") and machine in {"x86_64", "amd64"}:
        return "linux-x86_64"
    if sys.platform == "win32" and machine in {"x86_64", "amd64"}:
        return "windows-x86_64"
    return None


SUPPORTED_PLATFORM = _platform_key()
_WINDOWS_DLL_DIRECTORY_HANDLES = []


def _register_windows_dll_directories():
    if sys.platform != "win32":
        return
    directories = []
    configured = os.environ.get("ZLINK_LIBRARY_PATH")
    if configured:
        configured_path = pathlib.Path(configured).expanduser()
        directories.append(
            configured_path if configured_path.is_dir() else configured_path.parent
        )
    prefix = os.environ.get("ZLINK_CORE_PREFIX")
    if prefix:
        directories.append(pathlib.Path(prefix).expanduser() / "bin")
    directories.append(
        pathlib.Path(__file__).resolve().parent.parent
        / "native"
        / "windows-x86_64"
    )
    for directory in directories:
        if directory.is_dir():
            _WINDOWS_DLL_DIRECTORY_HANDLES.append(
                os.add_dll_directory(str(directory))
            )


_register_windows_dll_directories()


def _require_supported_platform():
    if SUPPORTED_PLATFORM is None:
        raise OSError(
            "zlink Python Core 0.14.3 supports Linux x86_64 and Windows x86_64"
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
            candidate_path = pathlib.Path(candidate).expanduser().resolve()
            if candidate_path.is_dir():
                candidate_path = candidate_path / "zlink.dll"
            if sys.platform == "win32":
                _WINDOWS_DLL_DIRECTORY_HANDLES.append(
                    os.add_dll_directory(str(candidate_path.parent))
                )
            lib = ctypes.CDLL(str(candidate_path))
            if bind is not None:
                bind(lib)
            return lib
        except (AttributeError, OSError) as exc:
            last_error = exc
    raise OSError("zlink native library not found or incompatible") from last_error


def _find_bundled_library():
    _require_supported_platform()
    base = pathlib.Path(__file__).resolve().parent.parent
    filename = "zlink.dll" if sys.platform == "win32" else "libzlink.so"
    candidate = base / "native" / SUPPORTED_PLATFORM / filename
    if candidate.exists():
        return str(candidate)
    return None


def _find_prefix_library():
    prefix_value = os.environ.get("ZLINK_CORE_PREFIX")
    if not prefix_value:
        return None

    prefix = pathlib.Path(prefix_value).expanduser()
    if sys.platform == "win32":
        candidate = prefix / "bin" / "zlink.dll"
    else:
        candidate = prefix / "lib" / "libzlink.so"
    if candidate.exists():
        return str(candidate)
    return None
