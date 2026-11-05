#!/usr/bin/env python3
"""사이트를 하나로 합치면서 자리가 바뀐 옛 경로를 새 자리로 보낸다.

core 사이트가 `zlink.systems`(Pages) 최상위였을 때의 주소는 이랬다.

  `/guide/...`     영어 가이드        → 지금은 같은 경로가 **한국어**다
  `/ko/guide/...`  한국어 가이드      → `/guide/...`
  `/api/...`       축약 API 레퍼런스  → `/reference/...`

합친 사이트는 한국어가 기본이라 `/ko/` 접두사가 없어졌고, 축약 `api/`를 버렸다.
그 두 자리에 stub을 깔아 외부 링크와 검색 색인을 잇는다.

`/api/...`는 처음에 정본 spec(`/spec/core/...`)으로 보냈다. spec은 이제 이 사이트에
싣지 않고 zlink-docs.zlink.systems에서 보므로(#3576), 같은 주제의 Core API 레퍼런스로
보낸다. 로그인이 필요한 자리로 보내면 외부 독자가 막힌다.

GitHub Pages는 정적 호스팅이라 301을 낼 수 없다. `<meta http-equiv="refresh">`와
`<link rel="canonical">`을 쓴다. 대상은 상대 경로다 — 사이트가 도메인 루트에 있든
`/zlink/` 아래에 있든 같은 자리를 가리킨다.

이미 페이지가 있는 자리는 덮지 않는다.

실행:
    python3 doc/site/scripts/make_core_redirects.py <사이트-루트>
"""

from __future__ import annotations

import sys
from pathlib import Path

STUB = """<!DOCTYPE html>
<html lang="ko">
<head>
<meta charset="utf-8">
<title>이동함</title>
<link rel="canonical" href="{target}">
<meta http-equiv="refresh" content="0; url={target}">
</head>
<body>
<p>문서가 <a href="{target}">여기</a>로 옮겨졌다.</p>
</body>
</html>
"""

#  옛 축약 API 레퍼런스 한 장 → Core API 레퍼런스의 대응 자리.
#  event는 monitor event와 readiness를 함께 다루던 장이라 socket monitor로 보낸다.
#  레퍼런스에 대응하는 장이 없는 spot은 레퍼런스 목차로, bindings는 bindings 가이드로 보낸다.
API_MAP = {
    "context": "reference/01-context",
    "message": "reference/02-message",
    "errors": "reference/18-errors",
    "errno-map": "reference/18-errors",
    "events": "reference/14-socket-monitor",
    "polling": "reference/15-polling",
    "monitoring": "reference/14-socket-monitor",
    "utilities": "reference/17-utilities",
    "socket": "reference/03-socket-lifecycle",
    "spot": "reference",
    "bindings": "bindings/guide",
    "README": "reference",
}


def write(root: Path, old_rel: str, new_rel: str) -> bool:
    old = root / old_rel / "index.html"
    if old.exists():
        return False                       # 실제 페이지가 있는 자리는 덮지 않는다
    up = "../" * len(Path(old_rel).parts)
    old.parent.mkdir(parents=True, exist_ok=True)
    old.write_text(STUB.format(target=f"{up}{new_rel}/"), encoding="utf-8")
    return True


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    root = Path(sys.argv[1]).resolve()
    if not (root / "index.html").exists():
        print(f"사이트 빌드 결과가 없다: {root}", file=sys.stderr)
        return 1

    written = 0
    #  `/ko/**` → 같은 경로의 한국어 기본판.
    #  옛 core 사이트가 `/ko/` 아래 두던 것은 이 셋뿐이다. framework 문서는
    #  그 사이트에 없었으므로 `/ko/dotnet/...` 같은 자리는 만들지 않는다.
    for top in ("guide", "internals"):
        base = root / top
        if not base.is_dir():
            continue
        for page in sorted(base.rglob("index.html")):
            rel = page.parent.relative_to(root)
            written += write(root, f"ko/{rel.as_posix()}", rel.as_posix())

    for old, new in API_MAP.items():
        written += write(root, f"api/{old}", new)
        written += write(root, f"ko/api/{old}", new)

    print(f"옛 경로 stub {written}개 생성")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
