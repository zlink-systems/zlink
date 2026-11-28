# SPDX-License-Identifier: MPL-2.0

from pathlib import Path


def test_readme_documents_caller_driven_completion():
    readme = (Path(__file__).resolve().parents[1] / "README.md").read_text(
        encoding="utf-8"
    )

    assert "daemon worker" not in readme
    assert "await submission.admitted" in readme
    assert "await submission.reply" in readme
    assert "samples/request_reply_async_sample.py" in readme
