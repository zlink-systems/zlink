"""문서에 적힌 저장소 상대 링크를 빌드 시점에 사이트 링크로 바꾼다.

문서는 GitHub에서도 그대로 읽힌다. 그래서 링크의 정본은 저장소 상대 경로이고
`scripts/check_doc_links.py`가 그 형태를 검사한다. 사이트는 정본 트리를
`docs/` 아래에 심링크로 모아 쓰므로 두 자리의 상대 경로가 서로 다르다. GitHub
에서 맞는 링크가 사이트에서는 다른 자리를 가리켜 404가 된다.

원본 마크다운을 사이트 기준으로 고치면 GitHub 뷰와 위 검사 스크립트가 함께
깨진다. 그래서 원본은 그대로 두고 빌드할 때 한 곳에서 바꾼다.

링크 대상을 현재 문서 소스의 realpath 기준으로 풀고, 결과에 따라 넷으로 나눈다.

1. 이번 로케일 사이트에 실리는 문서다 -> 현재 페이지 기준 상대 경로로 다시 쓴다.
2. 다른 로케일로 실리는 같은 문서다(문서 첫 줄의 언어 전환 링크가 여기 해당한다)
   -> 그 로케일의 사이트 절대 경로로 바꾼다. i18n plugin은 로케일마다 자기 파일
   집합만 들고 빌드하므로, 상대 경로로는 다른 로케일 문서를 가리킬 수 없다.
3. 저장소에는 있지만 사이트에 실리지 않는다(`exclude_docs`로 뺀 문서, `docs/`
   밖의 문서와 source file, 디렉터리) -> GitHub URL로 바꾼다.
4. 저장소에도 없다 -> 고치지 않고 warning을 낸다. `--strict`에서 빌드가 실패해야
   하는 진짜 오류다.

제외 목록과 심링크 표는 여기에 다시 적지 않는다. 사이트에 실리는지는 mkdocs가
넘겨주는 `files`로 판정하고, 정본 경로와 사이트 경로의 대응은 그 `files`의
realpath로 만든다. 로케일 목록과 로케일별 URL 접두는 i18n plugin 설정에서 읽고,
문서 URL은 mkdocs가 계산해 둔 `File.url`을 쓴다(`use_directory_urls` 설정과
`README.md` · `index.md`의 디렉터리 URL 접힘이 그대로 반영된다). 문서나 심링크나
로케일이 늘어도 이 파일은 그대로 둔다.
"""

from __future__ import annotations

import logging
import os
import posixpath
import re
from collections import Counter
from pathlib import Path
from urllib.parse import quote, urlsplit

#  GitHub URL이 가리키는 분기. 문서가 실려 나가는 자리라 릴리스 분기가 아니다.
BRANCH = "main"

#  이 파일은 `<repo>/doc/site/hooks/repo_links.py`에 있다.
REPO_ROOT = Path(__file__).resolve().parents[3]

log = logging.getLogger(f"mkdocs.hooks.{Path(__file__).stem}")

#  `[text](target)`과 `![alt](target)`. 저장소 문서에는 제목(`(path "title")`)이나
#  꺾쇠(`(<path>)`) 형태가 없으므로 대상은 공백 없는 한 덩어리다.
LINK_RE = re.compile(r"(?<=\]\()([^()\s]+)(?=\))")
FENCE_RE = re.compile(r"^\s*(```|~~~)")
INLINE_CODE_RE = re.compile(r"`[^`]*`")
#  스킴이 붙은 것, 앵커 전용, 사이트 절대 경로는 이미 제 자리를 가리킨다.
SKIP_RE = re.compile(r"^([a-zA-Z][a-zA-Z0-9+.\-]*:|#|/)")

#  분류별 변환 건수. 로케일마다 빌드가 한 번씩 돌아 누적된다.
_counts: Counter[str] = Counter()
#  같은 `files`를 페이지마다 다시 훑지 않는다.
_maps_cache: dict[int, tuple[dict, dict]] = {}


class _Locales:
    """i18n plugin이 아는 로케일과 로케일별 URL 접두."""

    def __init__(self, config) -> None:
        self.current: str | None = None
        self.links: dict[str, str] = {}
        plugin = config["plugins"].get("i18n")
        if plugin is None:
            return
        self.current = plugin.current_language
        #  `link`는 plugin이 로케일마다 채워 두는 절대 경로다(기본 로케일은 `/`,
        #  나머지는 `/<locale>/`). 값을 여기 다시 적지 않는다.
        for lang in plugin.config.languages:
            if lang.build:
                self.links[lang.locale] = lang.link

    def split(self, path: str) -> tuple[str, str] | None:
        """`<stem>.<locale>.md`를 `(stem, locale)`로 나눈다. 아니면 `None`."""
        base, ext = os.path.splitext(path)
        if ext.lower() != ".md":
            return None
        stem, dot, locale = base.rpartition(".")
        if not dot or locale not in self.links:
            return None
        return stem, locale


def _maps(files, locales: _Locales) -> tuple[dict, dict]:
    """(정본 realpath -> File, 로케일 접미사를 뗀 realpath -> File)."""
    cached = _maps_cache.get(id(files))
    if cached is not None:
        return cached

    by_path: dict[str, object] = {}
    by_stem: dict[str, object] = {}
    for f in files:
        if not f.abs_src_path or not f.inclusion.is_included():
            continue
        real = os.path.realpath(f.abs_src_path)
        by_path[real] = f
        split = locales.split(real)
        if split is not None:
            by_stem[split[0]] = f
    _maps_cache.clear()
    _maps_cache[id(files)] = (by_path, by_stem)
    return by_path, by_stem


def _already_resolves(files, page_uri: str, target: str) -> bool:
    """mkdocs가 지금 그대로도 대상을 찾는 링크인지 본다."""
    joined = posixpath.normpath(posixpath.join(posixpath.dirname(page_uri), target))
    if joined.startswith(".."):
        return False
    found = files.get_file_from_path(joined)
    return found is not None and found.inclusion.is_included()


def _repo_relative(abs_path: str) -> str | None:
    """저장소 안이면 저장소 기준 경로, 밖이면 `None`."""
    try:
        return Path(abs_path).relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return None


def _site_absolute(config, locales: "_Locales", locale: str, file) -> str:
    """다른 로케일로 실리는 같은 문서의 사이트 절대 경로.

    i18n plugin은 로케일마다 `File.url` 앞에 자기 접두를 붙여 둔다(`ko/...`).
    다른 로케일을 가리키려면 그 접두를 떼고 대상 로케일의 접두를 붙인다.
    """
    #  root 문서의 `File.url`은 `.`이다. 접두에 그대로 붙이면 `/ko/.`이 된다.
    url = "" if file.url in (".", "./") else file.url
    current = locales.links.get(locales.current, "/").lstrip("/")
    if current and url.startswith(current):
        url = url[len(current):]
    #  사이트가 도메인 root가 아닌 자리에 실릴 수 있다.
    base_url = urlsplit(config["site_url"] or "").path.rstrip("/")
    return f"{base_url}{locales.links[locale]}{url}"


def _rewrite(target: str, page_uri: str, page_dir: str, files, config,
             locales: _Locales, where: str) -> str:
    """링크 대상 하나를 바꾼다. 바꿀 수 없으면 그대로 돌려준다."""
    path, sep, anchor = target.partition("#")
    if not path:
        return target                      # 같은 문서 안의 앵커
    if _already_resolves(files, page_uri, path):
        return target

    abs_target = os.path.realpath(os.path.join(page_dir, path))
    by_path, by_stem = _maps(files, locales)

    split = locales.split(abs_target)
    if split is not None:
        stem, locale = split
        found = by_stem.get(stem)
        if found is not None and locale != locales.current:
            _counts["locale"] += 1
            return _site_absolute(config, locales, locale, found) + sep + anchor
        if found is not None:
            _counts["site"] += 1
            return _relative(found.src_uri, page_uri) + sep + anchor

    found = by_path.get(abs_target)
    if found is not None:
        _counts["site"] += 1
        return _relative(found.src_uri, page_uri) + sep + anchor

    rel_path = _repo_relative(abs_target)
    if rel_path is not None and os.path.exists(abs_target):
        is_dir = os.path.isdir(abs_target)
        _counts["tree" if is_dir else "blob"] += 1
        kind = "tree" if is_dir else "blob"
        repo_url = (config["repo_url"] or "").rstrip("/")
        return f"{repo_url}/{kind}/{BRANCH}/{quote(rel_path)}" + sep + anchor

    _counts["missing"] += 1
    log.warning(
        f"repo_links: {where} 의 링크 '{target}' 이 가리키는 "
        f"'{abs_target}' 이 저장소에 없다."
    )
    return target


def _relative(target_uri: str, page_uri: str) -> str:
    return posixpath.relpath(target_uri, posixpath.dirname(page_uri) or ".")


def on_page_markdown(markdown: str, page, config, files) -> str:
    if not config["repo_url"]:
        return markdown
    locales = _Locales(config)

    #  `abs_src_path`는 `docs/` 아래의 심링크 경로다. 링크는 정본 자리에 적혀
    #  있으므로 realpath를 먼저 구해야 `../../..`가 제대로 풀린다.
    page_dir = os.path.dirname(os.path.realpath(page.file.abs_src_path))
    page_uri = page.file.src_uri

    out = []
    in_fence = False
    for lineno, line in enumerate(markdown.splitlines(keepends=True), 1):
        if FENCE_RE.match(line):
            in_fence = not in_fence
            out.append(line)
            continue
        if in_fence or "](" not in line:
            out.append(line)
            continue

        #  인라인 code 안의 링크는 문법을 보여주는 예시라 건드리지 않는다.
        masked = [False] * len(line)
        for m in INLINE_CODE_RE.finditer(line):
            for i in range(m.start(), m.end()):
                masked[i] = True

        def repl(m: re.Match) -> str:
            target = m.group(1)
            if masked[m.start()] or SKIP_RE.match(target):
                return target
            return _rewrite(target, page_uri, page_dir, files, config, locales,
                            f"{page_uri}:{lineno}")

        out.append(LINK_RE.sub(repl, line))

    return "".join(out)


def on_post_build(config, **_) -> None:
    if not _counts:
        return
    log.info(
        "repo_links: 사이트 문서 %d건, 다른 로케일 문서 %d건, GitHub 파일 %d건, "
        "GitHub 디렉터리 %d건을 바꿨고 대상이 없는 링크 %d건을 남겼다."
        % (_counts["site"], _counts["locale"], _counts["blob"], _counts["tree"],
           _counts["missing"])
    )
    _counts.clear()
