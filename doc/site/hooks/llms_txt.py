"""빌드 nav를 언어별 llms.txt로 내보낸다."""

from __future__ import annotations

import re
from pathlib import Path
from urllib.parse import urljoin

from mkdocs.plugins import event_priority


_NAV_BY_LOCALE = {}
_SITE_SETTINGS = None
_SPEC_OR_INTERNAL_RE = re.compile(r"\b(?:spec|specifications?|internals?)\b", re.IGNORECASE)
_HTML_COMMENT_RE = re.compile(r"<!--.*?-->", re.DOTALL)
_MARKDOWN_LINK_RE = re.compile(r"!?\[([^\]]*)\]\([^)]*\)")


def _languages(config):
    """빌드할 로케일과 화면 표시 이름을 설정 순서대로 돌려준다."""
    plugin = config["plugins"].get("i18n")
    if plugin is None:
        return [("en", "English")]

    return [
        (language["locale"], language["name"])
        for language in plugin.config.languages
        if language.get("build", True)
    ]


def _is_excluded(item):
    title = getattr(item, "title", "") or ""
    if _SPEC_OR_INTERNAL_RE.search(title):
        return True

    if getattr(item, "is_page", False):
        source = getattr(getattr(item, "file", None), "src_uri", "") or ""
        segments = source.replace("\\", "/").lower().split("/")
        return any(segment in {"spec", "specs", "internals", "internal"} for segment in segments)

    return False


def _one_line(value):
    return " ".join(str(value).split()).strip()


def _description(page, trail):
    metadata = getattr(page, "meta", {}) or {}
    description = metadata.get("description")
    if isinstance(description, str) and _one_line(description):
        return _one_line(description)

    markdown = getattr(page, "markdown", None)
    if not isinstance(markdown, str):
        markdown = getattr(getattr(page, "file", None), "content_string", "")
    markdown = _HTML_COMMENT_RE.sub(" ", markdown or "")

    paragraph = []
    in_fence = False

    def summary(lines):
        text = _one_line(" ".join(lines))
        if not text:
            return ""
        if re.fullmatch(r"\[[^\]]+\]\([^)]*\)(?:\s*\|\s*\[[^\]]+\]\([^)]*\))*", text):
            return ""
        if text.startswith("View in another language") or ".zlink-langswitch" in text:
            return ""
        text = _MARKDOWN_LINK_RE.sub(r"\1", text)
        text = re.sub(r"`([^`]*)`", r"\1", text)
        text = re.sub(r"<[^>]*>", " ", text)
        text = re.sub(r"[*_~]", "", text)
        return _one_line(text)

    for raw_line in markdown.splitlines():
        line = raw_line.strip()
        if re.match(r"^(```|~~~)", line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        if not line:
            candidate = summary(paragraph)
            if candidate:
                return candidate[:240].rstrip()
            paragraph = []
            continue
        if (line.startswith("#") or line.startswith("---") or line.startswith("|")
                or line.startswith("<!--") or line.startswith("!!!") or line.startswith("???")
                or line.startswith("{") or line.startswith("![") or line.startswith(">")):
            continue
        if re.fullmatch(r"\[[^\]]+\]\([^)]*\)(?:\s*\|\s*\[[^\]]+\]\([^)]*\))*", line):
            continue
        if line.startswith("View in another language") or ".zlink-langswitch" in line:
            continue
        if line.startswith("<") and line.endswith(">"):
            continue
        paragraph.append(line)

    candidate = summary(paragraph)
    if candidate:
        return candidate[:240].rstrip()

    context = " / ".join(trail)
    return f"{context} documentation." if context else "ZLink documentation."


def _absolute_url(site_url, page):
    base = site_url if site_url.endswith("/") else f"{site_url}/"
    page_url = (getattr(page, "url", "") or "").lstrip("/")
    return urljoin(base, page_url)


def _escape_label(value):
    return str(value).replace("\\", "\\\\").replace("[", "\\[").replace("]", "\\]")


def _render_items(items, site_url, level=3, trail=()):
    lines = []
    page_count = 0

    for item in items:
        if _is_excluded(item):
            continue

        if getattr(item, "is_section", False):
            title = str(getattr(item, "title", "") or "").strip()
            nested, nested_count = _render_items(
                getattr(item, "children", ()) or (), site_url, level + 1, trail + (title,)
            )
            if not nested_count:
                continue
            if lines:
                lines.append("")
            lines.extend([f"{'#' * min(level, 6)} {_escape_label(title)}", ""])
            lines.extend(nested)
            page_count += nested_count
            continue

        if not getattr(item, "is_page", False):
            continue

        title = getattr(item, "title", None) or getattr(item, "url", "")
        description = _description(item, trail)
        lines.append(
            f"- [{_escape_label(title)}]({_absolute_url(site_url, item)}): {description}"
        )
        page_count += 1

    return lines, page_count


def render_llms_txt(site_name, site_description, site_url, navigations, languages):
    """언어별 MkDocs nav를 llms.txt 형식의 Markdown으로 직렬화한다."""
    lines = [f"# {_escape_label(site_name)}", "", f"> {_one_line(site_description)}", ""]
    total_pages = 0

    for locale, language_name in languages:
        nav = navigations.get(locale)
        if nav is None:
            raise ValueError(f"nav for configured locale '{locale}' was not captured")
        content, count = _render_items(nav, site_url)
        if not count:
            raise ValueError(f"nav for locale '{locale}' contains no public pages")
        lines.extend([f"## {_escape_label(language_name)}", "", *content, ""])
        total_pages += count

    if not total_pages:
        raise ValueError("MkDocs nav contains no public pages")
    return "\n".join(lines).rstrip() + "\n"


def on_config(config):
    """기본 로케일 설정을 저장한다. 내부 로케일별 빌드는 덮어쓰지 않는다."""
    global _SITE_SETTINGS

    plugin = config["plugins"].get("i18n")
    current = getattr(plugin, "current_language", None)
    default = getattr(plugin, "default_language", None)
    if _SITE_SETTINGS is None or plugin is None or current == default:
        _SITE_SETTINGS = {
            "site_name": config["site_name"],
            "site_description": config.get("site_description", ""),
            "site_url": config.get("site_url", ""),
        }
    return config


@event_priority(-200)
def on_nav(nav, *, config, files):
    """i18n의 nav 번역이 끝난 뒤 해당 로케일 nav를 보관한다."""
    plugin = config["plugins"].get("i18n")
    locale = getattr(plugin, "current_language", None) or "en"
    _NAV_BY_LOCALE[locale] = nav
    return nav


@event_priority(-200)
def on_post_build(*, config):
    """i18n의 모든 로케일 빌드가 끝난 뒤 사이트 루트에 파일을 쓴다."""
    plugin = config["plugins"].get("i18n")
    if plugin is not None and getattr(plugin, "building", False):
        return

    settings = _SITE_SETTINGS or {
        "site_name": config["site_name"],
        "site_description": config.get("site_description", ""),
        "site_url": config.get("site_url", ""),
    }
    content = render_llms_txt(
        settings["site_name"],
        settings["site_description"],
        settings["site_url"],
        _NAV_BY_LOCALE,
        _languages(config),
    )
    output = Path(config.site_dir) / "llms.txt"
    output.write_text(content, encoding="utf-8", newline="\n")
