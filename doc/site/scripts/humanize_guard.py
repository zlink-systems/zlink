#!/usr/bin/env python3
"""윤문(humanize) 회귀 가드.

문서를 자연스러운 한국어로 윤문할 때, 코드·식별자·링크·스니펫·탭 같은
"보호 토큰"이 바뀌면 빌드/정확성이 깨진다. 이 스크립트는 각 파일의 git HEAD
버전과 작업 트리 버전에서 보호 토큰을 추출해 **멀티셋이 동일한지** 비교한다.
산문(보호 토큰 밖 텍스트) 변경은 허용하고, 보호 토큰의 추가/삭제/변경만 실패로 본다.

보호 토큰 범주:
  - 펜스 코드블록 본문(``` ... ```)
  - 인라인 코드(`...`)
  - 링크 타깃(](...))
  - pymdownx 스니펫 지시자(--8<-- "...")
  - 탭 라벨(=== "...")
  - C/매크로 식별자(zlink_*, ZLINK_*)

사용:
    python3 doc/site/scripts/humanize_guard.py <file> [<file> ...]
보호 토큰이 바뀐 파일이 있으면 비-0으로 종료하고 차이를 출력한다.
"""

from __future__ import annotations

import re
import subprocess
import sys
from collections import Counter

FENCE = re.compile(r"```.*?\n(.*?)```", re.S)
INLINE = re.compile(r"`[^`\n]+`")
LINK = re.compile(r"\]\(([^)]+)\)")
SNIPPET = re.compile(r"--8<--\s*\"[^\"]+\"")
TAB = re.compile(r'^===\s+"[^"]+"', re.M)
IDENT = re.compile(r"\b(?:zlink_[a-z0-9_]+|ZLINK_[A-Z0-9_]+)\b")


def extract(text: str) -> dict[str, Counter]:
    return {
        "fenced-code": Counter(m.group(1) for m in FENCE.finditer(text)),
        "inline-code": Counter(INLINE.findall(text)),
        "link-target": Counter(LINK.findall(text)),
        "snippet": Counter(SNIPPET.findall(text)),
        "tab-label": Counter(TAB.findall(text)),
        "identifier": Counter(IDENT.findall(text)),
    }


def head_version(path: str) -> str | None:
    try:
        return subprocess.run(
            ["git", "show", f"HEAD:{path}"],
            capture_output=True, text=True, check=True,
        ).stdout
    except subprocess.CalledProcessError:
        return None  # new file — nothing to compare


def main(paths: list[str]) -> int:
    failures = 0
    for path in paths:
        head = head_version(path)
        if head is None:
            print(f"[skip] {path} (HEAD에 없음 — 신규 파일)")
            continue
        try:
            work = open(path, encoding="utf-8").read()
        except OSError as e:
            print(f"[err] {path}: {e}")
            failures += 1
            continue
        before, after = extract(head), extract(work)
        diffs = []
        for cat in before:
            removed = before[cat] - after[cat]
            added = after[cat] - before[cat]
            for tok, n in removed.items():
                diffs.append(f"    - [{cat}] 사라짐 ×{n}: {tok[:80]!r}")
            for tok, n in added.items():
                diffs.append(f"    + [{cat}] 새로생김 ×{n}: {tok[:80]!r}")
        if diffs:
            failures += 1
            print(f"[FAIL] {path} — 보호 토큰 변경 {len(diffs)}건:")
            print("\n".join(diffs))
        else:
            print(f"[ok]   {path} — 보호 토큰 동일(산문만 변경)")
    if failures:
        print(f"\n윤문 가드 실패: {failures}개 파일에서 보호 토큰이 바뀜.")
        return 1
    print("\n윤문 가드 통과: 모든 보호 토큰(코드·식별자·링크·스니펫·탭) 보존.")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: humanize_guard.py <file> [<file> ...]", file=sys.stderr)
        raise SystemExit(2)
    raise SystemExit(main(sys.argv[1:]))
