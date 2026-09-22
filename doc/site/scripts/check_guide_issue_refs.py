#!/usr/bin/env python3
"""Fail when a common guide links to an issue that has already closed.

The check uses the GitHub CLI when it is authenticated.  In environments without it, an available
``GITHUB_TOKEN`` is enough to use the GitHub REST API.  A source checkout without either is not a
failure: the script reports that the network check was skipped and exits successfully.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from urllib.error import URLError
from urllib.request import Request, urlopen


REPO_ROOT = Path(__file__).resolve().parents[3]
GUIDE_DIR = REPO_ROOT / "framework" / "doc" / "framework" / "common" / "guide"
ISSUE_RE = re.compile(r"https://github\.com/zlink-systems/zlink/issues/(\d+)")


def referenced_issues() -> dict[int, list[Path]]:
    found: dict[int, list[Path]] = {}
    for path in sorted(GUIDE_DIR.rglob("*.md")):
        for number in ISSUE_RE.findall(path.read_text(encoding="utf-8")):
            found.setdefault(int(number), []).append(path.relative_to(REPO_ROOT))
    return found


def gh_state(number: int) -> str:
    result = subprocess.run(
        ["gh", "issue", "view", str(number), "--repo", "zlink-systems/zlink", "--json", "state"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=True,
    )
    return json.loads(result.stdout)["state"]


def gh_is_authenticated() -> bool:
    if shutil.which("gh") is None:
        return False
    result = subprocess.run(
        ["gh", "auth", "status", "--hostname", "github.com"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    return result.returncode == 0


def api_state(number: int, token: str) -> str:
    request = Request(
        f"https://api.github.com/repos/zlink-systems/zlink/issues/{number}",
        headers={"Accept": "application/vnd.github+json", "Authorization": f"Bearer {token}"},
    )
    with urlopen(request, timeout=15) as response:  # nosec B310: fixed GitHub API endpoint
        return json.load(response)["state"].upper()


def main() -> int:
    issues = referenced_issues()
    if not issues:
        print("OK — common guides do not link to GitHub issues")
        return 0

    use_gh = gh_is_authenticated()
    token = os.environ.get("GITHUB_TOKEN")
    if not use_gh and not token:
        print("SKIP — neither authenticated gh nor GITHUB_TOKEN is available for guide issue checks")
        return 0

    closed: list[tuple[int, list[Path]]] = []
    try:
        for number, paths in issues.items():
            state = gh_state(number) if use_gh else api_state(number, token)
            if state.upper() == "CLOSED":
                closed.append((number, paths))
    except (subprocess.CalledProcessError, KeyError, json.JSONDecodeError, URLError) as error:
        if token and use_gh:
            try:
                for number, paths in issues.items():
                    if api_state(number, token).upper() == "CLOSED":
                        closed.append((number, paths))
            except (KeyError, URLError) as api_error:
                print(f"guide issue check failed: {api_error}", file=sys.stderr)
                return 2
        else:
            print(f"guide issue check failed: {error}", file=sys.stderr)
            return 2

    if closed:
        print("closed GitHub issues are still linked from common guides:", file=sys.stderr)
        for number, paths in closed:
            for path in paths:
                print(f"  #{number}: {path}", file=sys.stderr)
        return 1

    print(f"OK — {len(issues)} common-guide issue reference(s) point to open issues")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
