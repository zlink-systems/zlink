#!/usr/bin/env python3
"""격식체 회귀 가드 — 원칙 7.7이 이름을 든 낱말이 산문에 다시 들어오는 것을 막는다.

`documentation-principles.ko.md` 7.7의 두 표는 사석 동사·의인화·비유를 낱말까지 적어 두고
대체어를 함께 정한다. 그 표가 있어도 문서마다 사람이 읽어 잡으면 빠지는 곳이 생긴다.
이 스크립트는 그 표의 낱말을 목록으로 들고 저장소의 한글 산문을 훑는다.

세는 범위
  - 코드 블록(``` … ```) 안은 세지 않는다. 원칙 7.6이 산문에만 적용한다고 정한다.
  - 원칙 문서와 작성 가이드 자신은 이 낱말을 예시로 싣고 있으므로 제외한다.
  - spec·internals·e2e는 소유가 다른 문서라 기본 대상이 아니다. `--all`로 함께 본다.

사용
    python3 doc/site/scripts/check_register.py [--all]
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]

#  (낱말, 대체어). 대체어는 7.7의 2단계 표가 적은 것을 그대로 옮겼다.
BANNED: list[tuple[str, str]] = [
    ("가져간다", "맡는다 · 받는다"),
    ("끌어온다", "도입한다"),
    ("끌어와", "도입해"),
    #  `얹-`은 활용형이 많다. 어간으로 잡고 대체어는 문맥이 정한다.
    ("얹", "추가한다 · 적용한다 · 더한다"),
    ("더한다", "추가한다 · 포함한다"),
    ("더하는", "추가하는"),
    ("깊게 판다", "깊이 집중한다"),
    ("눈에는", "입장에서는"),
    ("빠르고 좋다", "측정한 값으로 적는다"),
    ("갈린다", "달라진다"),
    ("갈래", "형태 · 종류"),
    ("흔들린다", "불안정하다"),
    ("먹는다", "점유한다"),
    ("짚는다", "지적한다"),
    ("부르는 쪽", "호출하는 쪽"),
    #  "~라고 부른다"(이름 붙이기)는 대상이 아니다. 호출의 뜻만 잡는다.
    ("을 부른다", "을 호출한다"),
    ("를 부른다", "를 호출한다"),
    ("서로 부른다", "서로 호출한다"),
    ("한 번 부른다", "한 번 호출한다"),
    ("만 부른다", "만 호출한다"),
    ("쓸어 담", "한 번에 함께 처리한다"),
    ("털어낸다", "마무리한다"),
    ("떠안", "책임으로 남는다 · 직접 처리한다"),
    ("스며든다", "함께 들어온다"),
    ("밀어내", "지연시킨다"),
    ("밀어줘", "전달한다"),
    ("한 몸에", "함께 제공한다"),
    ("내놓는다", "제공한다"),
    ("껍데기", "상태를 갖지 않는다"),
    ("조립", "직접 만든다 · 구성한다"),
]

#  이 낱말들을 예시로 싣는 문서. 여기서는 규칙이 대상 자체다.
EXEMPT_NAMES = {
    "documentation-principles.ko.md",
    "guide-writing-guide.ko.md",
    "spec-writing-guide.ko.md",
    "source-comment-principles.ko.md",
    Path(__file__).name,
}

DEFAULT_ROOTS = ["framework/doc/framework", "bindings/doc", "core/doc/guide"]
WIDER_SKIP = ("/spec/", "/internals/", "/e2e/", "/doc/site/site/")


def prose_lines(path: Path):
    """코드 블록 밖의 줄만 (줄번호, 본문)으로 낸다."""
    in_fence = False
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if line.strip().startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        yield number, line


def main(argv: list[str]) -> int:
    wider = "--all" in argv
    findings: list[str] = []
    scanned = 0

    for root in DEFAULT_ROOTS:
        base = REPO / root
        if not base.exists():
            continue
        for path in sorted(base.rglob("*.ko.md")):
            posix = path.as_posix()
            if path.name in EXEMPT_NAMES:
                continue
            if not wider and any(part in posix for part in WIDER_SKIP):
                continue
            if "/doc/site/site/" in posix:
                continue
            scanned += 1
            for number, line in prose_lines(path):
                for word, instead in BANNED:
                    if word in line:
                        rel = path.relative_to(REPO).as_posix()
                        findings.append(
                            f"  - {rel}:{number}: `{word}` → {instead}\n"
                            f"      {line.strip()[:110]}")
                        break

    print(f"검사: 한글 산문 문서 {scanned}개")
    if findings:
        print(f"격식체 회귀 {len(findings)}건:")
        print("\n".join(findings))
        print("\n대체어는 documentation-principles.ko.md 7.7의 2단계 표가 소유한다.")
        return 1
    print("OK — 7.7이 이름을 든 낱말이 산문에 없다")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
