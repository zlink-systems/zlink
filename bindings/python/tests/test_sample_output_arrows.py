# SPDX-License-Identifier: MPL-2.0

import ast
from pathlib import Path

import pytest


SAMPLES = Path(__file__).resolve().parents[1] / "samples"


@pytest.mark.parametrize(
    "sample", sorted(SAMPLES.glob("*_sample.py")), ids=lambda path: path.name
)
def test_sample_output_uses_ascii_arrows(sample):
    tree = ast.parse(sample.read_text(encoding="utf-8"))
    prints = (
        node for node in ast.walk(tree)
        if isinstance(node, ast.Call)
        and isinstance(node.func, ast.Name)
        and node.func.id == "print"
    )
    for call in prints:
        for node in ast.walk(call):
            if isinstance(node, ast.Constant) and isinstance(node.value, str):
                assert "→" not in node.value
