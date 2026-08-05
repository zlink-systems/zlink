#!/usr/bin/env python3
"""공통 정본에서 언어별 가이드 장을 생성한다.

공통 12장은 `=== "Java"` 같은 언어 탭으로 다섯 언어를 한 파일에 담는다. 그 파일은
**소스**다 — 사람이 읽는 자리가 아니다. 읽는 자리는 언어별 디렉터리이고, 이 스크립트가
소스에서 그 언어의 탭만 남겨 만든다.

탭을 읽는 자리로 두면 독자가 자기 언어와 무관한 코드를 60~67% 함께 본다. 장마다 탭 띠가
끼어들어 흐름도 끊긴다. 생성판은 그 언어의 코드만 담으므로 원본의 43% 분량이고 탭이 없다.

생성 규칙
  - `=== "라벨"` 블록 중 대상 언어만 남기고 4칸 들여쓰기를 푼다.
  - 머리에 생성 파일 표시를 붙인다. 손으로 고치면 다음 생성에서 지워진다.
  - 산문의 표면 이름을 그 언어 표기로 바꾼다(`surface_terms.py`). 공통 산문은 다섯
    언어가 공유하므로 `.NET` 모양으로 적혀 있는데, 생성판은 대상이 정해져 있다.
  - `` `11. Monitoring` 장 `` 표기를 **실제 링크**로 바꾼다. 공통 소스는 대상 언어가
    정해지지 않아 링크할 수 없지만, 생성판은 정해져 있다.
  - 그 언어의 읽는 순서대로 앞뒤 장 nav를 붙인다. 순서는 각 언어 README의 표가 소유한다.

`common/guide/server/*.ko.md`와 `*.en.md`를 각각 독립된 로케일로 처리한다. 두 언어 모두
같은 파일 이름 규칙(`NN-slug.<ko|en>.md`)을 쓰고, 언어별 디렉터리에 같은 확장자로 생성한다.
소스 디렉터리에 어느 한쪽 로케일 파일이 아직 없으면 그 로케일은 조용히 건너뛴다 — `en`
소스가 아직 없는 초기 상태에서도 `ko` 생성은 그대로 동작해야 한다.

상대 링크는 손대지 않는다. `common/guide/server/`와 `<lang>/guide/server/`는 깊이가 같아
`../../../common/spec/...` 같은 링크가 그대로 성립한다.

실행:
    python3 doc/site/scripts/generate_language_guides.py           # 생성
    python3 doc/site/scripts/generate_language_guides.py --check   # 최신인지만 확인
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import surface_terms  # noqa: E402

SITE_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = SITE_DIR.parents[1]
FRAMEWORK = REPO_ROOT / "framework" / "doc" / "framework"
COMMON = FRAMEWORK / "common" / "guide" / "server"

#  탭 라벨 → 언어 디렉터리.
LANGUAGES = {
    "C#/.NET": "dotnet",
    "C++": "cpp",
    "Java": "java",
    "Kotlin": "kotlin",
    "Node/TypeScript": "node",
}

#  로케일별 산문 문자열. 생성판 자체가 en/ko 어느 쪽이냐에 따라 안내 문구도 그 언어로
#  나가야 읽는 사람이 어색하지 않다.
STRINGS = {
    "ko": {
        "front_matter": "---\ntitle: \"{title} · {label}\"\n---\n\n",
        "banner": (
            "<!-- generated:start -->\n"
            "<!-- 이 파일은 `common/guide/server/{source}`에서 생성한다."
            " 직접 고치지 않는다.\n"
            "     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/"
            "generate_language_guides.py`로 다시 만든다. -->\n"
            "<!-- generated:end -->\n"
        ),
        "switch_label": "다른 언어로 보기",
        "guide_home": "가이드 홈",
        "prev": "이전",
        "next": "다음",
        "chapter_ref": re.compile(r"`(\d\d)\. ([^`]+)` 장"),
        "redirect_ext": re.compile(r"\]\(([^)#\s]+\.ko\.md)\)"),
        "no_order_table": "{readme}에 읽는 순서 표가 없다",
    },
    "en": {
        "front_matter": "---\ntitle: \"{title} · {label}\"\n---\n\n",
        "banner": (
            "<!-- generated:start -->\n"
            "<!-- This file is generated from `common/guide/server/{source}`."
            " Do not edit directly.\n"
            "     Edit the common source instead, then regenerate with"
            " `python3 doc/site/scripts/"
            "generate_language_guides.py`. -->\n"
            "<!-- generated:end -->\n"
        ),
        "switch_label": "View in another language",
        "guide_home": "Guide Home",
        "prev": "Previous",
        "next": "Next",
        "chapter_ref": re.compile(r"`(\d\d)\. ([^`]+)` chapter"),
        "redirect_ext": re.compile(r"\]\(([^)#\s]+\.en\.md)\)"),
        "no_order_table": "{readme} has no reading-order table",
    },
}

TAB_RE = re.compile(r'^=== +"(.+)"\s*$')
ORDER_RE = re.compile(r"^\| \d+ \| \[[^\]]+\]\(([^)]+)\)")
NAV_RE = re.compile(
    r"<!-- framework-adapter-nav:start -->.*?<!-- framework-adapter-nav:end -->\n",
    re.S)


def language_switch(current: str, name: str, suffix: str) -> str:
    """같은 장의 다른 언어로 가는 줄.

    탭에서는 한 번 눌러 언어를 바꿀 수 있었다. 생성판은 언어별 파일이라 그 경로가
    사라진다. 장마다 이 줄을 붙여 되살린다. 저장소에서 읽을 때도 그대로 동작한다.
    """
    parts = []
    for label, lang in LANGUAGES.items():
        if lang == current:
            parts.append(f"**{label}**")
        else:
            parts.append(f"[{label}](../../../{lang}/guide/server/{name})")
    return ("<!-- language-switch:start -->\n"
            f"{STRINGS[suffix]['switch_label']} — " + " · ".join(parts)
            + "\n<!-- language-switch:end -->\n\n")


def reading_order(lang_dir: str, suffix: str) -> list[str]:
    """그 언어 README의 읽는 순서 표에서 장 파일 목록을 읽는다."""
    readme = FRAMEWORK / lang_dir / "guide" / "server" / f"README.{suffix}.md"
    if not readme.exists():
        raise SystemExit(STRINGS[suffix]["no_order_table"].format(readme=readme))
    order = []
    for line in readme.read_text(encoding="utf-8").splitlines():
        m = ORDER_RE.match(line)
        if m:
            order.append(m.group(1))
    if not order:
        raise SystemExit(STRINGS[suffix]["no_order_table"].format(readme=readme))
    return order


def chapter_title(path: Path) -> str:
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("# "):
            return line[2:].strip()
    return path.stem


def strip_tabs(text: str, label: str) -> str:
    """대상 언어 탭만 남기고 들여쓰기를 푼다."""
    lines = text.splitlines()
    out: list[str] = []
    i = 0
    while i < len(lines):
        if not TAB_RE.match(lines[i]):
            out.append(lines[i])
            i += 1
            continue
        keep: list[str] | None = None
        while i < len(lines):
            m = TAB_RE.match(lines[i])
            if not m:
                break
            current = m.group(1)
            i += 1
            body: list[str] = []
            while i < len(lines):
                line = lines[i]
                if TAB_RE.match(line):
                    break
                #  탭 본문은 4칸 들여쓴다. 들여쓰기가 끝나면 그룹도 끝난다.
                if line.strip() and not line.startswith("    "):
                    break
                body.append(line[4:] if line.startswith("    ") else line)
                i += 1
            if current == label:
                keep = body
        if keep is None:
            continue
        while keep and not keep[0].strip():
            keep.pop(0)
        while keep and not keep[-1].strip():
            keep.pop()
        out.extend(keep)
        out.append("")
    return "\n".join(out)


def link_chapter_refs(text: str, available: dict[str, str], suffix: str) -> str:
    """`` `11. Monitoring` 장/chapter `` → 실제 링크."""
    def repl(m: re.Match) -> str:
        number, title = m.group(1), m.group(2)
        target = available.get(number)
        if target is None:
            return m.group(0)
        return f"[{number}. {title}]({target})"
    return STRINGS[suffix]["chapter_ref"].sub(repl, text)


#  Kotlin 가이드는 Java 런타임을 공유하므로 Java 문서를 그대로 가리키는 장이 있다
#  (16-options, internals). 공통 소스는 형제 링크로 쓰지만 Kotlin 디렉터리에는 그
#  파일이 없다. Java 쪽으로 돌려 준다.
SHARES_WITH = {"kotlin": "java"}


def redirect_missing(text: str, lang_dir: str, suffix: str) -> str:
    donor = SHARES_WITH.get(lang_dir)
    if donor is None:
        return text
    target_dir = FRAMEWORK / lang_dir / "guide" / "server"
    donor_dir = FRAMEWORK / donor / "guide" / "server"

    def repl(m: re.Match) -> str:
        link = m.group(1)
        if (target_dir / link).resolve().exists():
            return m.group(0)
        if not (donor_dir / link).resolve().exists():
            return m.group(0)
        #  `../../internals/x.md`처럼 경로를 낀 링크도 있다. 붙여 쓰면 중간에
        #  `guide/server/../..`가 남아 읽기 어려우므로 정규화한다.
        resolved = (donor_dir / link).resolve().relative_to(FRAMEWORK)
        depth = len(target_dir.relative_to(FRAMEWORK).parts)
        return m.group(0).replace(link, "../" * depth + str(resolved))

    return STRINGS[suffix]["redirect_ext"].sub(repl, text)


def nav_block(order: list[str], name: str, titles: dict[str, str], suffix: str) -> str:
    """그 언어의 읽는 순서 기준 앞뒤 장 nav."""
    try:
        index = order.index(name)
    except ValueError:
        return ""
    s = STRINGS[suffix]
    parts = [f"[{s['guide_home']}](README.{suffix}.md)"]
    if index > 0:
        prev = order[index - 1]
        parts.append(f"[{s['prev']}: {titles.get(prev, prev)}]({prev})")
    if index + 1 < len(order):
        nxt = order[index + 1]
        parts.append(f"[{s['next']}: {titles.get(nxt, nxt)}]({nxt})")
    return ("<!-- framework-adapter-nav:start -->\n"
            + " | ".join(parts)
            + "\n<!-- framework-adapter-nav:end -->\n\n")


def generate_locale(suffix: str, check_only: bool) -> tuple[int, list[str]]:
    sources = sorted(COMMON.glob(f"*.{suffix}.md"))
    if not sources:
        return 0, []

    s = STRINGS[suffix]
    stale: list[str] = []
    written = 0
    for label, lang_dir in LANGUAGES.items():
        target_dir = FRAMEWORK / lang_dir / "guide" / "server"
        raw_order = reading_order(lang_dir, suffix)
        #  순서 표는 공통 장을 `../../../common/guide/server/...`로 가리킨다. 그 장들은
        #  이 디렉터리에 생성되므로 형제가 된다. 다른 언어를 가리키는 항목(kotlin의
        #  Java 16장)만 외부 링크로 남는다.
        order: list[str] = []
        for entry in raw_order:
            name = Path(entry).name
            if (target_dir / name).exists() or (COMMON / name).exists():
                order.append(name)
            else:
                order.append(entry)
        siblings = [entry for entry in order if "/" not in entry]
        #  번호 → 형제 파일. 장 참조를 링크로 바꿀 때 쓴다.
        available = {name[:2]: name for name in siblings}

        titles: dict[str, str] = {}
        for entry in order:
            name = Path(entry).name
            local = target_dir / name
            src = COMMON / name
            path = local if local.exists() else src
            if path.exists():
                titles[entry] = chapter_title(path)

        generated = {src.name for src in sources} & set(siblings)
        for src in sources:
            if src.name not in generated:
                continue
            body = strip_tabs(src.read_text(encoding="utf-8"), label)
            body = NAV_RE.sub("", body)
            body, _ = surface_terms.translate(body, label)
            body = link_chapter_refs(body, available, suffix)
            body = redirect_missing(body, lang_dir, suffix)
            content = (s["front_matter"].format(
                           title=chapter_title(src), label=label)
                       + s["banner"].format(source=src.name) + "\n"
                       + nav_block(order, src.name, titles, suffix)
                       + language_switch(lang_dir, src.name, suffix) + body)
            content = re.sub(r"\n{3,}", "\n\n", content).rstrip() + "\n"

            out = target_dir / src.name
            previous = out.read_text(encoding="utf-8") if out.exists() else None
            if previous == content:
                continue
            if check_only:
                stale.append(str(out.relative_to(REPO_ROOT)))
                continue
            out.write_text(content, encoding="utf-8")
            written += 1

    return written, stale


def generate(check_only: bool) -> int:
    total_written = 0
    total_stale: list[str] = []
    total_sources = 0
    for suffix in ("ko", "en"):
        written, stale = generate_locale(suffix, check_only)
        total_written += written
        total_stale.extend(stale)
        total_sources += len(list(COMMON.glob(f"*.{suffix}.md")))

    if check_only:
        if total_stale:
            print(f"생성물이 소스와 다르다 {len(total_stale)}건:", file=sys.stderr)
            for path in total_stale:
                print(f"  - {path}", file=sys.stderr)
            print("\n`python3 doc/site/scripts/generate_language_guides.py`를 "
                  "실행하고 결과를 커밋한다.", file=sys.stderr)
            return 1
        print("OK — 언어별 가이드가 공통 소스와 일치한다")
        return 0

    print(f"생성: {total_written}개 갱신 "
          f"(언어 {len(LANGUAGES)} × 공통 소스 파일 {total_sources}개)")
    return 0


if __name__ == "__main__":
    raise SystemExit(generate("--check" in sys.argv[1:]))
