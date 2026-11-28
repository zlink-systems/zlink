# SPDX-License-Identifier: MPL-2.0

from pathlib import Path


def test_readme_points_to_core_release_metadata():
    root = Path(__file__).resolve().parents[1]
    readme = (root / "README.md").read_text(encoding="utf-8")

    assert "(../../VERSION)" in readme
    assert "Core 0.17.0" not in readme
    assert (root / "../../VERSION").is_file()
