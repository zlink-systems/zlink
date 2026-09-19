#!/usr/bin/env python3
"""README의 빌드·실행·검증 절에서 플랫폼별 명령 블록을 그대로 뽑아 온다.

#655 CI guard(.github/workflows/standalone-zips.yml)가 쓴다. 이 guard는 저장소를
checkout하지 않고 배포된 zip만 받아 돌리므로, 무엇을 실행할지는 그 zip 안 README가
정해야 한다 — workflow 파일에 손으로 옮겨 적은 명령은 README가 바뀌어도 따라가지
않는다.

## README가 지킬 규칙 (제안, #655)

각 zip 루트의 `README.ko.md`(정본)는 다음 절 이름을 그대로 쓴다:
전제 조건 / 내려받기와 설치 / 빌드 / 실행 / 검증 / 문제 해결

`빌드`·`실행`·`검증` 절 안에서 이 script가 뽑아야 하는 명령마다, 여는 fence에
`title="<platform>"`을 붙인다. `<platform>`은 `linux` 또는 `windows`다. 이미 이
저장소의 문서 site(mkdocs-material)가 코드 block title을 렌더링하므로 사람이 읽을 때도
그대로 쓸모 있다 — CI만 위한 표시가 아니다.

    ## 빌드

    \x60\x60\x60bash title="linux"
    dotnet build Tutorial.sln
    \x60\x60\x60

    \x60\x60\x60powershell title="windows"
    dotnet build Tutorial.sln
    \x60\x60\x60

절 하나에 같은 platform의 fence가 여럿이면 첫 번째만 쓴다. 다음 `## ` 제목이 나오면
그 절은 끝난다.

사용:
    python3 scripts/tutorial/extract_readme_step.py <README 경로> <절 이름> <linux|windows>

찾으면 그 블록의 내용을 그대로 stdout에 낸다. 절이나 platform을 찾지 못하면 exit 2와
stderr 메시지 — 새 README 규칙이 아직 그 zip에 없다는 뜻이며, 호출자는 이를 오늘의
결함이 아니라 "아직 없음"으로 다뤄야 한다(예: 기존 하드코딩 명령으로 fallback).
"""
from __future__ import annotations

import pathlib
import re
import sys

FENCE_RE = re.compile(
    r'^```(?P<lang>\S+)\s+title="(?P<platform>[a-z]+)"\s*$'
)
SECTION_RE = re.compile(r'^##\s+(?P<title>.+?)\s*$')


def extract(text: str, section: str, platform: str) -> str | None:
    lines = text.splitlines()
    in_section = False
    i = 0
    while i < len(lines):
        line = lines[i]
        heading = SECTION_RE.match(line)
        if heading:
            in_section = heading.group("title") == section
            i += 1
            continue
        if in_section:
            fence = FENCE_RE.match(line)
            if fence and fence.group("platform") == platform:
                body = []
                i += 1
                while i < len(lines) and not lines[i].startswith("```"):
                    body.append(lines[i])
                    i += 1
                return "\n".join(body) + "\n"
        i += 1
    return None


def main() -> int:
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} <README> <section> <linux|windows>", file=sys.stderr)
        return 2
    readme_path, section, platform = sys.argv[1], sys.argv[2], sys.argv[3]
    if platform not in ("linux", "windows"):
        print(f"::error::platform must be linux or windows, got {platform!r}", file=sys.stderr)
        return 2

    path = pathlib.Path(readme_path)
    if not path.is_file():
        print(f"no such README: {readme_path}", file=sys.stderr)
        return 2

    block = extract(path.read_text(encoding="utf-8"), section, platform)
    if block is None:
        print(
            f"no '{platform}'-titled fenced block found under section '## {section}' in {readme_path} "
            "(README does not follow the #655 convention yet)",
            file=sys.stderr,
        )
        return 2

    sys.stdout.write(block)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
