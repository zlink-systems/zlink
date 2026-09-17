"""i18n 플러그인의 `navigation.instant` 경고 하나를 내리깐다.

경고 내용은 이렇다 — `navigation.instant`를 켜면 로케일 전환기(한국어/English)의 "같은
문서" 링크가 낡는다. 헤더가 다시 그려지지 않아, 빌드 때 박힌 링크가 처음 연 문서의 것으로
굳기 때문이다.

**그 링크는 `assets/javascripts/zlink-lang.js`의 `fixLocaleLinks()`가 이동마다 다시
계산한다.** 주소 규칙이 단순해서다 — 기본 로케일 `en`은 접두사가 없고 `ko`는 `/ko/`를
갖는다. 그래서 이 저장소에서는 해당 경고가 가리키는 문제가 실제로 일어나지 않는다.

`--strict`는 경고를 오류로 올리므로, 이 한 줄만 골라 내린다. 문구로 좁게 걸러 같은
플러그인의 다른 경고는 그대로 통과시킨다.

**필터는 로거가 아니라 핸들러에 건다.** 플러그인은 `mkdocs.plugins.*` 하위 로거에 적고,
로거에 붙인 필터는 하위에서 올라온 기록에는 적용되지 않는다. `--strict`의 판정도 기록이
핸들러에 닿은 뒤에 이뤄지므로 그 앞에서 잘라야 한다.

전환기를 손대거나 그 스크립트를 지운다면 이 훅도 함께 지워야 한다.
"""

from __future__ import annotations

import logging

NEEDLE = (
    "language switcher contextual link is not compatible with "
    "theme.features = navigation.instant"
)


class _Drop(logging.Filter):
    def filter(self, record: logging.LogRecord) -> bool:
        try:
            return NEEDLE not in record.getMessage()
        except Exception:
            return True


_FILTER = _Drop()


def _install() -> None:
    root = logging.getLogger()
    targets = list(root.handlers)
    for name in ("mkdocs", "mkdocs.commands.build"):
        targets.extend(logging.getLogger(name).handlers)
    for handler in targets:
        if _FILTER not in handler.filters:
            handler.addFilter(_FILTER)


def on_startup(command, dirty, **kwargs):
    _install()


def on_config(config, **kwargs):
    #  핸들러는 빌드가 시작될 때 붙기도 한다. 한 번 더 건다(중복은 걸러진다).
    _install()
    return config
