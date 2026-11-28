"""Tests for the package selector shared by CI and pre-pr."""

from argparse import Namespace
from contextlib import redirect_stdout
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile


spec = importlib.util.spec_from_file_location(
    "binding_package", Path(__file__).with_name("framework-binding-package.py"))
binding_package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(binding_package)


class BindingPackageTest(unittest.TestCase):
    def test_framework_only_allows_reuse(self):
        with patch.object(binding_package, "git", return_value=b"") as git:
            self.assertFalse(binding_package.binding_inputs_changed("origin/main"))
        for call in git.call_args_list:
            self.assertEqual(call.args[-3:], ("--", "bindings/", "core/"))

    def test_checkout_for_binding_commit_or_worktree_change(self):
        for outputs in ((b"bindings/cpp/CMakeLists.txt\x00",),
                        (b"", b"?? bindings/cpp/new.cpp\x00"),
                        (b"", b" M core/CMakeLists.txt\x00")):
            with self.subTest(outputs=outputs), patch.object(binding_package, "git", side_effect=outputs):
                self.assertTrue(binding_package.binding_inputs_changed("origin/main"))

    def assert_cpp_fallback(self, archive, expected):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cache = root / "cache"
            cache.mkdir()
            (cache / "cache-key.zip").write_bytes(archive)
            args = Namespace(base="origin/main", language="cpp", destination=root / "out",
                             build=["checkout-build"], publish=False)
            message = io.StringIO()
            with (patch.dict(os.environ, ZLINK_LOCAL_BINDING_CACHE=str(cache)),
                  patch.object(binding_package, "binding_inputs_changed", return_value=False),
                  patch.object(binding_package, "cpp_cache_name", return_value="cache-key"),
                  patch.object(binding_package.subprocess, "run") as run,
                  redirect_stdout(message)):
                binding_package.prepare(args)
            run.assert_called_once_with(["checkout-build"], check=True)
            self.assertIn(f"{expected}; building from checkout", message.getvalue())
            self.assertFalse((cache / "cache-key.zip").exists())

    def test_cpp_corrupt_cache_falls_back_to_checkout(self):
        self.assert_cpp_fallback(b"invalid ZIP", "File is not a zip file")

    def test_cpp_version_or_digest_mismatch_falls_back_to_checkout(self):
        package_file = "include/zlink.hpp"
        for key, expected in (
            ("wrong-version", "C++ binding cache key or version mismatch"),
            ("cache-key", f"C++ binding digest mismatch for {package_file}"),
        ):
            with self.subTest(key=key):
                data = io.BytesIO()
                with zipfile.ZipFile(data, "w") as archive:
                    archive.writestr("manifest.json", json.dumps(
                        {"key": key, "sha256": {package_file: "wrong-digest"}}))
                    archive.writestr(f"package/{package_file}", b"header")
                self.assert_cpp_fallback(data.getvalue(), expected)


if __name__ == "__main__":
    unittest.main()
