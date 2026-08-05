#!/usr/bin/env python3
"""공통 산문의 표면 이름을 언어별 표기로 바꾼다.

공통 산문은 다섯 언어가 공유하므로 타입·메서드 이름을 부를 수 없다(게이트 3). 그래서
`InstanceSpot(...)`처럼 `.NET` 표기가 중립인 척 남아 있고, 바로 아래 Java 코드는
`.instanceSpot()`이라 읽는 쪽이 둘을 잇지 못한다.

생성 단계가 생겼으므로 여기서 해결한다. 산문의 backtick 식별자를 그 언어의 표기로
바꾼다. 표기 규칙은 언어 관례를 따르고, 관례로 안 되는 자리만 표로 적는다.

**추측하지 않는다.** 바꾼 이름이 그 언어의 실제 표면에 없으면 바꾸지 않고 그대로 둔다.
근거는 `check_guide_identifiers.py`가 쓰는 것과 같다 — 그 언어의 framework 소스와
공개 계약 spec이다. 그래서 이 치환은 "있는 이름으로만" 바뀐다.
"""

from __future__ import annotations

import re
from pathlib import Path

SITE_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = SITE_DIR.parents[1]
SPEC_DIR = REPO_ROOT / "framework" / "doc" / "framework" / "common" / "spec"

#  이름이 표면인지 판정하는 근거. **샘플은 넣지 않는다** — 샘플의 도메인 이름
#  (`TicTacToeGame`·`CommerceApi`)까지 표면으로 보면 엉뚱한 치환이 생긴다.
FRAMEWORK_SOURCES = {
    "C#/.NET": (["framework/languages/dotnet/src"], [".cs"]),
    "C++": (["framework/languages/cpp/framework"], [".hpp", ".cpp"]),
    "Java": (["framework/languages/java"], [".java"]),
    "Kotlin": (["framework/languages/java"], [".kt", ".java"]),
    "Node/TypeScript": (["framework/languages/node/packages"], [".ts"]),
}

SPEC_DIRS = {
    "C#/.NET": ["server/languages/dotnet"],
    "C++": ["server/languages/cpp"],
    "Java": ["server/languages/java"],
    "Kotlin": ["server/languages/kotlin", "server/languages/java"],
    "Node/TypeScript": ["server/languages/node"],
}

#  관례로 안 되는 자리. 이름 자체가 갈리므로 표가 소유한다.
#  키는 공통 산문이 쓰는 표기(`.NET` 모양)다.
OVERRIDES: dict[str, dict[str, str]] = {
    #  Java·Kotlin의 Spot handler 등록은 종류별 메서드가 아니라 addHandler 하나다.
    #  무엇을 받는 handler인지는 구현한 interface와 annotation이 정한다.
    "AddPacket": {"Java": "addHandler", "Kotlin": "addHandler"},
    "AddSubscribe": {"Java": "addHandler", "Kotlin": "addHandler"},
    "AddActorPacket": {"Java": "addHandler", "Kotlin": "addHandler"},
}

#  spec이 소유하는 언어 중립 용어와 도메인 단어. 치환 대상이 아니다.
NEUTRAL = {
    #  공개 계약이 정의한 식별자·상태 값. 언어가 달라도 같은 낱말을 쓴다.
    "ChannelName", "MeshName", "NodeRid", "SpotId", "ActorId", "RoutingId",
    "ObjectGeneration", "AuthorityOwnerGeneration", "OwnerLeaseGeneration",
    "NodeGeneration", "SpotWide", "PerActor", "DeadlineExceeded", "NotFound",
    "Unavailable", "ShuttingDown", "InvalidOperation", "Blocked", "Created",
    "Existing", "Rejected", "Relocated", "Relocating", "Draining", "Stopped",
    "Serving", "Preparing", "Error", "ForceStopped", "Ready", "None", "Client",
    "Server", "Weight", "Scoped", "Transient", "Singleton", "ZLink", "Zlink",
    #  결과·상태 값과 정책 enum. 이름 자체가 계약이고, 언어별 표기는 그 언어의
    #  enum 관례(Java·Kotlin은 SCREAMING_SNAKE)를 따르므로 낱말 치환 대상이 아니다.
    "Accepted", "Completed", "Failed", "Empty", "Immediate", "Manual", "Delay",
    "CatchUpBounded", "AnyTurnBoundary", "ApplicationSignaled", "RecreateOnRelocation",
    "PreserveStateWith", "DisableRelocation", "ExplicitClose", "HostShutdown",
    "RelocationOut", "Recovered",
    #  너무 흔한 낱말이라 산문에서 표면 이름으로 읽히지 않는다.
    "State", "Name", "Session", "Worker", "Period", "Deadline",
    #  §10 매핑표가 소유하는 terminal. 표기 차이를 설명하는 것이 산문의 일이다.
    "Async", "Submit", "submit", "Yield", "yield", "await",
}

IDENT_RE = re.compile(r"`([A-Z][A-Za-z0-9_]*)(\(\.\.\.\)|\(\))?`")
#  `.foo` 또는 `foo(` — 멤버 접근이나 호출·선언 자리. 단순 변수명은 걸리지 않는다.
MEMBER_RE = re.compile(r"(?:\.\s*([A-Za-z_]\w*)|\b([A-Za-z_]\w*)\s*\()")
#  선언된 타입 이름. 타입은 어느 언어에서도 PascalCase라 낱말을 바꾸면 안 된다.
#  `ActorRef`를 `actorRef`로 바꾸면 타입이 변수가 된다.
TYPE_DECL_RE = re.compile(
    r"\b(?:class|interface|record|struct|enum|type)\s+(?:I|ZLink|IZLink)?([A-Z]\w*)")

_cache: dict[str, set[str]] = {}
_types: set[str] | None = None


def _read(paths: list[str], exts: list[str]) -> str:
    chunks: list[str] = []
    for rel in paths:
        root = REPO_ROOT / rel
        if not root.is_dir():
            continue
        for ext in exts:
            for path in root.rglob(f"*{ext}"):
                if set(path.parts) & {
                        "node_modules", "build", "bin", "obj", "dist", ".gradle"}:
                    continue
                try:
                    chunks.append(path.read_text(encoding="utf-8", errors="ignore"))
                except OSError:
                    continue
    return "\n".join(chunks)


def surface(label: str) -> set[str]:
    """그 언어에 실제로 있는 식별자 집합."""
    if label in _cache:
        return _cache[label]
    paths, exts = FRAMEWORK_SOURCES[label]
    text = _read(paths, exts)
    for rel in SPEC_DIRS.get(label, []):
        root = SPEC_DIR / rel
        if root.is_dir():
            text += "\n" + "\n".join(
                p.read_text(encoding="utf-8", errors="ignore")
                for p in root.rglob("*.ko.md"))
    #  낱말이 아무 데나 한 번 나온 것으로 "표면에 있다"고 보면 `state`·`name`처럼
    #  흔한 변수명이 전부 통과한다. **멤버로 쓰인 자리**만 근거로 삼는다.
    found: set[str] = set()
    for dotted, called in MEMBER_RE.findall(text):
        found.add(dotted or called)
    _cache[label] = found
    return _cache[label]


def dotnet_types() -> set[str]:
    """`.NET`이 선언한 타입 이름. 접두사(`I`·`ZLink`)를 뗀 형태도 함께 담는다."""
    global _types
    if _types is None:
        paths, exts = FRAMEWORK_SOURCES["C#/.NET"]
        _types = set(TYPE_DECL_RE.findall(_read(paths, exts)))
    return _types


def _camel(name: str) -> str:
    return name[0].lower() + name[1:]


def _snake(name: str) -> str:
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower()


def candidates(name: str, label: str) -> list[str]:
    """그 언어에서 이 이름이 가질 수 있는 표기. 앞에 오는 것이 우선이다."""
    if label == "C#/.NET":
        return [name, name + "Async"]
    if label == "C++":
        base = _snake(name)
        return [base, base + "_t"]
    return [_camel(name), _camel(name) + "Async", name]


def translate(text: str, label: str) -> tuple[str, list[tuple[str, str]]]:
    """산문의 표면 이름을 그 언어 표기로 바꾼다. (결과, 바꾼 목록)."""
    known = surface(label)
    dotnet = surface("C#/.NET")
    changed: list[tuple[str, str]] = []

    def repl(m: re.Match) -> str:
        name, call = m.group(1), m.group(2) or ""
        if name in NEUTRAL:
            return m.group(0)
        #  `.NET` framework 표면에 없는 이름은 도메인 단어다. 건드리지 않는다.
        if name not in dotnet and name + "Async" not in dotnet:
            return m.group(0)
        #  타입 이름은 어느 언어에서도 PascalCase다. 낱말을 바꾸면 안 된다.
        if not call and name in dotnet_types():
            return m.group(0)
        override = OVERRIDES.get(name, {}).get(label)
        picked = override if override else next(
            (c for c in candidates(name, label) if c in known), None)
        if picked is None or picked == name:
            return m.group(0)
        changed.append((name + call, picked + call))
        return f"`{picked}{call}`"

    return IDENT_RE.sub(repl, text), changed
