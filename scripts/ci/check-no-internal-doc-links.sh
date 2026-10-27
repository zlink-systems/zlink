#!/usr/bin/env bash
# 공개 파일에서 비공개 저장소로 옮긴 문서의 옛 경로를 검사한다.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
python_cmd=python3
if ! command -v "$python_cmd" >/dev/null 2>&1; then
    python_cmd=python
fi
"$python_cmd" - <<'PY'
import posixpath
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit

# 이전 경로 목록은 이곳에서만 관리한다.
moved = {
    "doc/principal/dev/supervisor-guide.ko.md",
    "doc/principal/dev/workspace-notes.ko.md",
    "doc/principal/dev/worktree-setup.ko.md",
    "doc/principal/dev/development-workflow.ko.md",
    "doc/principal/dev/development-workflow.md",
    "doc/principal/dev/verification-principles.ko.md",
    "doc/plan/guide-rewrite.ko.md",
    "doc/plan/guide-06-spot-reorder.patch",
    "doc/building/release-accounts.ko.md",
    "doc/building/release-accounts.md",
    ".claude/agents/sonnet-coder.md",
}
self_path = "scripts/ci/check-no-internal-doc-links.sh"
basenames = {posixpath.basename(name) for name in moved}
reference_hint = re.compile("|".join(re.escape(name) for name in basenames)
                            + r"|github\.com/zlink-systems/docs")
paths = subprocess.check_output(
    ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"]
).decode("utf-8").split("\0")
token_pattern = re.compile(r"(?:[A-Za-z][A-Za-z0-9+.-]*://)?[A-Za-z0-9_./%~+\-]+")
failures = []
checked = 0
for name in sorted(set(paths)):
    path = Path(name)
    if not name or name == self_path or not path.is_file() or path.is_symlink():
        continue
    data = path.read_bytes()
    if b"\0" in data:
        continue
    checked += 1
    content = unquote(data.decode("utf-8", errors="replace"))
    if not reference_hint.search(content):
        continue
    for number, line in enumerate(content.splitlines(), 1):
        if not reference_hint.search(line):
            continue
        # Windows 경로도 같은 목록으로 판정한다.
        for match in token_pattern.finditer(line.replace("\\", "/")):
            token = unquote(match.group()).rstrip(".,;:")
            url = urlsplit(token)
            if url.scheme and url.netloc:
                if (url.netloc.lower() == "github.com"
                        and (url.path.rstrip("/") == "/zlink-systems/docs"
                             or url.path.startswith("/zlink-systems/docs/"))):
                    failures.append((name, number, "private GitHub URL"))
                    break
                # 공개 저장소의 옛 GitHub 링크도 더 이상 유효하지 않다.
                candidate = re.sub(r"^/zlink-systems/zlink/(?:blob|raw)/[^/]+/", "", url.path)
                candidates = {posixpath.normpath(candidate)}
            else:
                candidates = {
                    posixpath.normpath(token.lstrip("/")),
                    posixpath.normpath(posixpath.join(posixpath.dirname(name), token)),
                }
            if candidates & moved:
                failures.append((name, number, token))
                break

if failures:
    for name, number, target in failures:
        print(f"{name}:{number}: internal document reference: {target}")
    print(f"FAIL: {len(failures)} internal document reference(s)")
    sys.exit(1)
print(f"PASS: no internal document references ({checked} public text files checked)")
PY
