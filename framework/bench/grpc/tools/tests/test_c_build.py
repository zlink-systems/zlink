"""The C build entry point owns Release for every target, including protobuf."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class CBuildTest(unittest.TestCase):
    def test_release_is_passed_to_cmake(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            here = root / "framework/bench/grpc/c"
            here.mkdir(parents=True)
            (here / "build.sh").write_text((ROOT / "c/build.sh").read_text())
            (here.parent / "runner_common.sh").write_text("bench_require_low_load() { :; }\n")
            (root / "VERSION").write_text("LIBZLINK_VERSION=1.19.0\n")
            binary = root / "cmake"
            binary.write_text('#!/bin/sh\nprintf "%s\n" "$@" >> "$BUILD_ARGS"\n')
            binary.chmod(0o755)
            args = root / "args"
            environment = dict(os.environ, PATH=str(root) + ":" + os.environ["PATH"],
                               BUILD_ARGS=str(args), ZLINK_CORE_PACKAGE_PREFIX=str(root))
            subprocess.run(["bash", str(here / "build.sh")], env=environment, check=True,
                           capture_output=True)
            self.assertIn("-DCMAKE_BUILD_TYPE=Release", args.read_text().splitlines())
