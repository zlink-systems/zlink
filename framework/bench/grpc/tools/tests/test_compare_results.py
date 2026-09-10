"""Black-box tests for the before/after Markdown comparison command."""

from __future__ import annotations

import os
import subprocess
import sys
import unittest


TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(TOOLS, "compare-results.py")
FIXTURE = os.path.join(TOOLS, "tests", "fixtures", "compare-results")


class CompareResultsTest(unittest.TestCase):
    def run_compare(self, fixture: str) -> str:
        completed = subprocess.run(
            [
                sys.executable,
                SCRIPT,
                os.path.join(FIXTURE, fixture, "before"),
                os.path.join(FIXTURE, fixture, "after"),
            ],
            check=False,
            capture_output=True,
            encoding="utf-8",
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        return completed.stdout

    def test_renders_markdown_ratios_changes_depth_and_geomeans(self):
        output = self.run_compare("complete")
        self.assertIn("## Scenario × payload", output)
        self.assertIn("`zlink-framework-node` / request-window | 1024", output)
        self.assertIn("100.000 KOPS → 104.000 KOPS", output)
        self.assertIn("1.0400x | +4.00%", output)
        self.assertIn("## Cell evidence (5% measurement tolerance)", output)
        self.assertIn("within cell tolerance", output)
        self.assertIn("## Size geometric-mean evidence", output)
        self.assertIn("meets geometric-mean rule", output)
        self.assertIn("outside geometric-mean rule", output)
        self.assertIn("Depth before → after", output)

    def test_keeps_missing_error_abandoned_and_drain_states_visible(self):
        output = self.run_compare("mixed-state")
        self.assertIn("**missing**", output)
        self.assertIn("errors=2", output)
        self.assertIn("abandoned=3", output)
        self.assertIn("drain bound hit", output)
        self.assertIn("not comparable", output)
        self.assertIn("Geometric mean | Rule | Evidence", output)


if __name__ == "__main__":
    unittest.main()
