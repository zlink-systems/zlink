"""`docs/` 아래에 심링크로 둘 수 없는 정본 파일을 빌드할 때 복사한다.

`docs/`는 정본 트리를 심링크로 모아 쓴다. 디렉터리 심링크는 Windows에서도 만들어지지만,
**파일 심링크는 개발자 모드나 관리자 권한이 없으면 만들어지지 않는다.** `ln -s`가 조용히
복사본을 만들고, 그 복사본이 정본과 갈라진 채 남아 사이트가 옛 내용을 낸 적이 있다.

그래서 파일 단위로 붙이는 정본은 심링크가 아니라 이 훅이 빌드마다 복사한다.

**이 복사본을 커밋하지 않는다.** 저장소에서 이 경로들은 symlink(mode 120000)로 tracked라
`.gitignore`가 받지 못한다. Windows 체크아웃에서는 `git status`에 "modified"로 보이는데,
그대로 커밋하면 다른 환경의 심링크가 파일로 바뀐다. 커밋 전에 다음으로 되돌린다.

```bash
git checkout -- doc/site/docs/index.ko.md doc/site/docs/index.en.md \
                doc/site/docs/install.ko.md doc/site/docs/install.en.md \
                doc/site/docs/assets/korean.css
```

`doc/site/docs/assets/stylesheets/extra.css`는 심링크가 아니라 정본이다. 그쪽은 그대로
고치고 커밋한다.
"""

from __future__ import annotations

import shutil
from pathlib import Path

SITE = Path(__file__).resolve().parents[1]
DOCS = SITE / "docs"
REPO = SITE.parents[1]

#  (docs 안의 이름, 정본 경로)
MIRRORED = [
    ("index.ko.md", "framework/doc/framework/index.ko.md"),
    ("index.en.md", "framework/doc/framework/index.en.md"),
    ("install.ko.md", "framework/doc/framework/install.ko.md"),
    ("install.en.md", "framework/doc/framework/install.en.md"),
    ("assets/korean.css", "framework/doc/framework/assets/korean.css"),
]


def on_pre_build(config, **kwargs):
    for name, source in MIRRORED:
        src = REPO / source
        dst = DOCS / name
        if not src.exists():
            raise FileNotFoundError(f"mirror_files: 정본이 없다 — {src}")
        #  심링크로 남아 있으면 그대로 둔다. 링크가 만들어지는 환경에서는 그쪽이 낫다.
        if dst.is_symlink():
            continue
        #  내용이 같으면 쓰지 않는다. `mkdocs serve`의 감시기는 mtime 변경만 보고 재빌드를
        #  걸기 때문에, 무조건 복사하면 빌드 → 복사 → 빌드가 끝없이 돈다.
        if dst.exists() and dst.read_bytes() == src.read_bytes():
            continue
        shutil.copyfile(src, dst)
