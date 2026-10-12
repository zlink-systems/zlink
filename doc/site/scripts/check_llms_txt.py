#!/usr/bin/env python3
"""빌드된 llms.txt의 URL이 사이트 안의 실제 페이지를 가리키는지 검사한다."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path, PurePosixPath
from urllib.parse import unquote, urlsplit

SITE_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = SITE_DIR.parents[1]
LLMS_FILENAME = "llms.txt"
ENTRY_RE = re.compile(r"^- \[(?P<title>.+?)\]\((?P<url>[^)\s]+)\):\s*(?P<description>\S.*)$")


def configured_site_url():
    """MkDocs 설정에서 검증 기준 URL을 읽는다."""
    from mkdocs.config import load_config

    return load_config(config_file=str(SITE_DIR / "mkdocs.yml"))["site_url"]


def _target_file(site_dir: Path, site_url: str, target_url: str):
    if re.search(r"%(?![0-9A-Fa-f]{2})", target_url):
        return None, "URL contains an invalid percent escape"
    try:
        base = urlsplit(site_url)
        target = urlsplit(target_url)
    except ValueError:
        return None, "URL is malformed"
    if base.scheme not in {"http", "https"} or not base.netloc:
        return None, "configured site_url is not an absolute HTTP(S) URL"
    if target.scheme not in {"http", "https"} or not target.netloc:
        return None, "URL is not an absolute HTTP(S) URL"
    if (target.scheme.lower(), target.netloc.lower()) != (base.scheme.lower(), base.netloc.lower()):
        return None, "URL is outside site_url origin"
    if target.query or target.fragment:
        return None, "URL has a query or fragment and does not identify a built page"

    base_path = unquote(base.path or "/")
    if not base_path.endswith("/"):
        base_path += "/"
    target_path = unquote(target.path or "/")
    if target_path == base_path.rstrip("/"):
        relative_path = ""
    elif target_path.startswith(base_path):
        relative_path = target_path[len(base_path):]
    else:
        return None, "URL is outside site_url path"

    parts = PurePosixPath(relative_path).parts
    if any(part in {"..", "."} for part in parts):
        return None, "URL path escapes the built site directory"

    target_file = site_dir.joinpath(*parts)
    if not relative_path or target_path.endswith("/"):
        target_file /= "index.html"

    root = site_dir.resolve()
    resolved = target_file.resolve()
    try:
        resolved.relative_to(root)
    except ValueError:
        return None, "URL path escapes the built site directory"
    return resolved, None


def validate_site(site_dir, site_url):
    """검사 오류 목록을 돌려준다. 성공 조건은 오류가 없는 것이다."""
    site_dir = Path(site_dir)
    errors = []
    llms_path = site_dir / LLMS_FILENAME
    if not llms_path.is_file():
        return [f"missing {llms_path}"]

    entries = []
    for line_number, line in enumerate(llms_path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.startswith("-"):
            continue
        match = ENTRY_RE.fullmatch(line)
        if match is None:
            errors.append(f"{LLMS_FILENAME}:{line_number}: malformed page entry")
            continue
        entries.append((line_number, match.group("url")))

    if not entries:
        errors.append(f"{LLMS_FILENAME}: no page URLs found")

    for line_number, url in entries:
        target_file, error = _target_file(site_dir, site_url, url)
        if error:
            errors.append(f"{LLMS_FILENAME}:{line_number}: {url}: {error}")
        elif not target_file.is_file():
            errors.append(
                f"{LLMS_FILENAME}:{line_number}: {url}: built page does not exist ({target_file})"
            )

    return errors


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("site_dir", type=Path, help="MkDocs build output directory")
    parser.add_argument(
        "--site-url", help="canonical MkDocs site_url (defaults to doc/site/mkdocs.yml)"
    )
    args = parser.parse_args(argv)
    site_url = args.site_url or configured_site_url()
    errors = validate_site(args.site_dir, site_url)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    llms_path = args.site_dir / LLMS_FILENAME
    entry_count = sum(1 for line in llms_path.read_text(encoding="utf-8").splitlines()
                      if line.startswith("- ["))
    print(f"llms.txt: {entry_count} page URLs resolve under {site_url}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
