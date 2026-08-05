"""mermaid 다이어그램 안의 글씨를 키운다.

글씨 크기만 올려서는 거의 안 커진다. mermaid는 폰트 크기로 레이아웃을 계산한
뒤 그 결과를 컨테이너 폭에 맞춰 통째로 축소하기 때문이다. 폰트를 25% 키우면
자연폭도 함께 늘어 축소율이 그만큼 나빠진다.

다만 완전히 상쇄되지는 않는다. 노드 사이 간격(`nodeSpacing`·`rankSpacing`)과
노드 안쪽 여백(`padding`)은 폰트와 무관한 상수라 그대로 있기 때문이다. 그래서
**폰트를 키우면서 그 상수들을 줄이면** 자연폭은 오히려 줄고 글씨만 커진다.
축소율이 좋아진 만큼이 화면에서 보이는 크기 차이가 된다.

`wrappingWidth`를 줄이면 긴 라벨이 더 일찍 줄바꿈해 노드가 좁고 높아진다.
가로가 모자란 그림에서 폭을 버는 쪽으로 작동한다.

값은 아래 상수 하나로 모아 뒀다. 더 키우려면 폰트를 올리고 간격을 더 줄인다.
"""

from __future__ import annotations

import re

FONT_SIZE = "18px"

#  다이어그램 타입 → (mermaid 설정 키, 그 키에 넣을 항목).
#  간격 상수는 타입마다 이름이 다르므로 한 번에 못 준다.
LAYOUT = {
    "flowchart": ("flowchart", "'nodeSpacing': 32, 'rankSpacing': 40, "
                               "'padding': 8, 'wrappingWidth': 180"),
    "graph": ("flowchart", "'nodeSpacing': 32, 'rankSpacing': 40, "
                           "'padding': 8, 'wrappingWidth': 180"),
    #  sequence는 폰트 크기를 themeVariables가 아니라 자기 설정으로 받는다.
    #  Material이 16px로 초기화해 두므로 여기서 덮어야 한다.
    "sequenceDiagram": ("sequence", f"'actorFontSize': '{FONT_SIZE}', "
                                    f"'messageFontSize': '{FONT_SIZE}', "
                                    f"'noteFontSize': '{FONT_SIZE}', "
                                    "'boxMargin': 8, 'width': 140"),
    "stateDiagram-v2": ("state", "'nodeSpacing': 32, 'rankSpacing': 40, 'padding': 8"),
    "stateDiagram": ("state", "'nodeSpacing': 32, 'rankSpacing': 40, 'padding': 8"),
}

FENCE_RE = re.compile(r"(^(?P<indent>[ \t]*)```+mermaid[ \t]*\n)(?P<body>.*?)(?=^[ \t]*```)",
                      re.S | re.M)
INIT_OPEN_RE = re.compile(r"^([ \t]*%%\{\s*init\s*:\s*\{)")
THEME_OPEN_RE = re.compile(r"'themeVariables'\s*:\s*\{")


def _diagram_type(body: str) -> str | None:
    for line in body.splitlines():
        s = line.strip()
        if not s or s.startswith("%%"):
            continue
        return s.split()[0].rstrip(";")
    return None


def _inject(body: str, layout_key: str, layout_body: str) -> str:
    """`%%{init}%%` 디렉티브에 설정을 병합한다. 없으면 새로 놓는다."""
    theme = f"'themeVariables': {{'fontSize': '{FONT_SIZE}'}}"
    layout = f"'{layout_key}': {{{layout_body}}}"

    lines = body.splitlines(keepends=True)
    for i, line in enumerate(lines):
        m = INIT_OPEN_RE.match(line)
        if not m:
            continue
        if "fontSize" in line or "nodeSpacing" in line or "actorFontSize" in line:
            return body          # 문서가 직접 정한 값은 건드리지 않는다
        rest = line[m.end(1):]
        #  themeVariables가 이미 있으면 그 안에 넣는다. 같은 키를 두 번 쓰면
        #  뒤엣것이 앞엣것을 통째로 덮어 기존 설정이 사라진다.
        tm = THEME_OPEN_RE.search(rest)
        if tm:
            rest = (rest[:tm.end()] + f"'fontSize': '{FONT_SIZE}', "
                    + rest[tm.end():])
            lines[i] = m.group(1) + layout + ", " + rest
        else:
            lines[i] = m.group(1) + layout + ", " + theme + ", " + rest
        return "".join(lines)

    indent = re.match(r"[ \t]*", lines[0]).group(0) if lines else ""
    return f"{indent}%%{{init: {{{layout}, {theme}}}}}%%\n" + body


def on_page_markdown(markdown: str, **_) -> str:
    def repl(m: re.Match) -> str:
        body = m.group("body")
        entry = LAYOUT.get(_diagram_type(body) or "")
        return m.group(1) + (_inject(body, *entry) if entry else body)

    return FENCE_RE.sub(repl, markdown)
