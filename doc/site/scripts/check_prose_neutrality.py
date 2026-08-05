#!/usr/bin/env python3
"""공통 정본 산문에 특정 언어 고유 이름이 남았는지 검사한다(런북 §11 게이트 3).

공통 12장은 다섯 언어가 한 파일을 공유한다. 산문이 한 언어의 타입·메서드 이름을
부르면 나머지 넷의 독자에게는 없는 이름이 된다. 런북 §5.2가 정한 역할 분담은 이렇다.

  탭 밖 산문  — 개념, 동작 순서, 판단 기준, 제약
  탭 안 코드  — 그 언어의 실제 호출
  코드 주석   — 코드만으로 안 보이는 그 언어의 제약

따라서 **탭 밖 산문만** 본다. 탭 안 코드와 그 주석은 언어별이므로 정상이다.

검사 대상에서 빼는 것:
  - 코드 펜스 안(탭 안팎 무관)
  - 탭 블록(`=== "라벨"` 아래 들여쓴 줄 전체)
  - `ALLOWED`에 등록한 이름 — 다섯 언어 공통 규칙으로 spec이 소유하는 용어다

실행:
    python3 doc/site/scripts/check_prose_neutrality.py
위반이 있으면 비-0으로 종료한다.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

SITE_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = SITE_DIR.parents[1]
COMMON_GUIDE = REPO_ROOT / "framework" / "doc" / "framework" / "common" / "guide"

FENCE_RE = re.compile(r"^\s*(```|~~~)")
TAB_RE = re.compile(r'^=== +"')

#  언어를 드러내는 이름 꼴. 하나라도 산문에 있으면 그 언어 독자만 읽을 수 있다.
PATTERNS = [
    (".NET · Java · Node 타입 접두사", re.compile(r"\bI?ZLink[A-Z]\w*")),
    ("C++ 타입 접미사", re.compile(r"\b[a-z][a-z0-9_]*_t\b")),
    (".NET 표면", re.compile(
        r"\b(IServiceCollection|IHostedService|ValueTask|CancellationToken"
        r"|ILogger|ActivitySource|IHostApplicationLifetime"
        r"|IApplicationBuilder|WebApplicationBuilder)\b")),
    ("Java 표면", re.compile(
        r"\b(CompletionStage|CompletableFuture|ExecutorService"
        r"|SpringApplication)\b")),
    ("Node 표면", re.compile(
        r"\b(AbortSignal|NestJS|@Injectable|@Inject|@Module)\b")),
    ("Kotlin 표면", re.compile(r"\b(suspend fun|StateFlow|CoroutineScope)\b")),
    #  `.NET` terminal이 붙은 operation 이름. 계약 문서가 정한 언어 중립 이름은
    #  terminal을 뗀 쪽이다(spec 20 §4.1이 NotifyDisconnected로 명시한다).
    (".NET terminal이 붙은 operation 이름", re.compile(r"\b\w+Async\b")),
]

#  다섯 언어 공통 규칙이라 산문에 나와도 되는 이름. 근거를 함께 적는다.
ALLOWED = {
    #  비동기 실행 정책 spec이 소유하는 terminal 이름. 다섯 언어의 표기 차이를
    #  설명하는 것이 산문의 일이라 이름 자체가 본문에 나온다.
    "Async", "submit", "Submit", "Yield", "yield", "await",
    #  spec이 정의한 계기·wire 이름이라 언어 무관이다.
    "zlink",
    #  "Spring과 NestJS 위에도 똑같이 올라간다"처럼 **다른 언어를 예로 드는** 문장에
    #  나온다. 어느 언어 독자가 봐도 맞는 서술이라 언어별로 바꾸지 않는다.
    "NestJS",
}

#  operation 이름을 산문에 적을 때는 terminal을 뗀 언어 중립 이름을 쓴다. spec 20 §4.1이
#  이 규약을 정한다 — "이 언어 중립 operation을 `NotifyDisconnected`라 하며 `.NET` exact
#  interface에서는 `NotifyDisconnectedAsync(...)`로 표현한다."

#  검사에서 통째로 빼는 파일. 이유를 함께 적는다.
SKIP_FILES = {
    #  언어별 산출물과 패키지를 나열하는 것이 이 장의 본질이다.
    "14-samples.ko.md",
}


def prose_lines(path: Path):
    """(줄 번호, 산문 줄). 코드 펜스와 탭 블록은 건너뛴다."""
    in_fence = False
    in_tab = False
    for ln, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if FENCE_RE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        if TAB_RE.match(line):
            in_tab = True
            continue
        if in_tab:
            #  탭 본문은 들여쓴다. 들여쓰기가 끝나면 탭 블록도 끝난다.
            if line.strip() and not line.startswith("    "):
                in_tab = False
            else:
                continue
        yield ln, line


def main() -> int:
    docs = sorted(p for p in COMMON_GUIDE.rglob("*.md")
                  if p.name not in SKIP_FILES)
    if not docs:
        print(f"검사할 공통 정본이 없다: {COMMON_GUIDE}", file=sys.stderr)
        return 2

    errors: list[str] = []
    checked = 0
    for md in docs:
        rel = md.relative_to(REPO_ROOT)
        for ln, line in prose_lines(md):
            checked += 1
            for label, pattern in PATTERNS:
                for m in pattern.finditer(line):
                    if m.group(0) in ALLOWED:
                        continue
                    errors.append(f"[{label}] {rel}:{ln}: {m.group(0)}")

    print(f"검사: 공통 정본 {len(docs)}개, 산문 {checked}줄")
    if errors:
        print(f"\n언어 고유 이름 {len(errors)}건:", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1
    print("OK — 공통 정본 산문이 언어 고유 이름을 담지 않는다")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
