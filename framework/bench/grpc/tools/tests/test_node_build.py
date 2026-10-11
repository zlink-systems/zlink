"""Repository workspace preparation belongs to the Node bench build entrypoint."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

COMMON = Path(__file__).resolve().parents[2] / "runner_common.sh"


class NodeBuildTest(unittest.TestCase):
    def test_node_build_prepares_workspace_before_build(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            commands = root / "npm.log"
            npm = root / "npm"
            npm.write_text('#!/bin/bash\nprintf "%s\\n" "$*" >>"$NPM_LOG"\n',
                           encoding="utf-8")
            npm.chmod(0o755)
            node = root / "node"
            node.write_text(npm.read_text(), encoding="utf-8")
            node.chmod(0o755)
            env = dict(os.environ, PATH=f"{root}:{os.environ['PATH']}", NPM_LOG=str(commands))
            subprocess.run(["bash", str(COMMON.parent / "node/build.sh")], env=env,
                           check=True, capture_output=True)
            calls = commands.read_text().splitlines()
            workspace = str(COMMON.parents[2] / "languages/node")
            prepare = f"{workspace}/scripts/materialize-local-http-client-package.mjs"
            self.assertIn(prepare, calls)
            self.assertLess(calls.index(prepare), calls.index(f"--prefix {workspace} ci"))
            self.assertIn(f"--prefix {workspace} ci", calls)
            self.assertLess(calls.index(f"--prefix {workspace} ci"), calls.index("run build"))
