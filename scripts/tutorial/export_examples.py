#!/usr/bin/env python3
"""quickstart·tutorial·samples를 언어별 examples 저장소 tree로 내보낸다.

독자는 `git clone https://github.com/zlink-systems/zlink-<lang>-examples`로 셋을 한 번에 받는다
(#831). 원본은 이 저장소의 `framework/languages/<lang>/{quickstart,tutorial,samples}` 하나이고,
미러 저장소에는 `examples-mirror.yml`이 framework 릴리스 태그와 `main` push마다 이 script의 출력을
커밋한다(릴리스된 버전만, 규칙은 그 workflow 머리말).
이 script는 그 커밋에 들어갈 tree를 만드는 것까지만 한다 — push는 workflow가 한다.

출력은 `<출력_디렉터리>/zlink-<lang>-examples/`이고 그 안에 `quickstart/`·`tutorial/`·`samples/`와
저장소 루트 파일(README 둘, `.gitignore`, `.gitattributes`)이 놓인다. `examples-smoke.yml`은
push 전에 같은 tree를 만들어 checkout 없는 job에서 README 명령만으로 빌드·실행한다.

파일 목록은 `git ls-tree`가 정하므로 커밋된 파일만 들어가고 `bin`·`obj`·`node_modules`·`dist`를
제외하는 규칙을 따로 두지 않는다. 그 규칙은 `.gitignore`가 이미 갖고 있고, 같은 파일을 미러
루트에 그대로 복사하므로 독자의 빌드 산출물도 같은 규칙으로 무시된다.

**`git archive`가 그대로 내지 못하는 것 둘을 여기서 바로잡는다.**

- **줄바꿈.** Windows에서 내보내면 `core.autocrlf`가 끼어들어 `gradlew` 같은 shell script까지
  CRLF가 되고, 그 tree를 WSL이나 Linux에서 쓰면 `bad interpreter: /bin/sh^M`으로 죽는다.
  반대로 `.bat`·`.cmd`·`.ps1`은 CRLF여야 한다. `.gitattributes`가 정한 규칙을 tree에 적용한다.
- **실행 권한.** `git archive --format=zip`이 POSIX mode를 싣지 않아 `gradlew`와 `*.sh`가
  실행되지 않는다. tree에 쓸 때 mode를 직접 준다(Windows에서는 무의미하고, Linux runner에서
  git이 그 mode를 커밋한다).

사용:
    python scripts/tutorial/export_examples.py [출력_디렉터리] [git_ref]

기본값은 `.artifacts/examples`와 `HEAD`다.
"""
from __future__ import annotations

import io
import os
import pathlib
import posixpath
import re
import shutil
import subprocess
import sys
import zipfile

# Java와 Kotlin은 원본 Gradle project를 공유한다. export는 공통 root 파일을 각각 넣고,
# 한 규칙으로 source tree와 settings의 include를 언어별로 걸러 독립 Gradle project를 만든다.
LANGUAGES = ("cpp", "dotnet", "java", "kotlin", "node")

#  Engine examples are repositories by product rather than by language. Each source subtree
#  becomes the mirror root without an extra Server/, Unity/, or Unreal/ directory.
ENGINE_EXPORTS = {
    "Server": "zlink-engine-server",
    "Unity": "zlink-unity-examples",
    "Unreal": "zlink-unreal-examples",
    "Godot": "zlink-godot-examples",
    "Axmol": "zlink-axmol-examples",
    "CocosCreator": "zlink-cocos-creator-examples",
}

#  미러 루트에 놓이는 디렉터리. 저장소 경로 `framework/languages/<lang>/<section>`과 같다.
SECTIONS = ("quickstart", "tutorial", "samples")

#  README에 적는 언어 이름.
LANGUAGE_TITLES = {
    "cpp": "C++", "dotnet": ".NET", "java": "Java", "kotlin": "Kotlin", "node": "Node",
}

#  저장소 안에서만 의미가 있고 미러 밖에서는 쓰이지 않거나 오히려 방해가 되는 파일.
#  (section, lang, git ls-tree 기준 subtree 상대 경로) 튜플이며, 항목마다 근거를 남긴다.
#  #655에서 언어별 worker가 보고한 목록이다 — 새 항목은 여기 추가하고 이유를 적는다.
EXCLUDED_ENTRIES: tuple[tuple[str, str, str], ...] = (
    # 저장소 CI가 로컬에서만 돌리는 준비 script. 미러 밖에서는 참조할 대상이 없어
    # 그대로 두면 독자가 실행 가능한 진입점으로 착각한다.
    ("tutorial", "dotnet", "ci-steps.local.sh"),
    # 저장소에서 실행해 본 흔적이 남은 로그 파일. 빌드 산출물이 아니라 실수로 커밋된 것.
    ("tutorial", "dotnet", "run.out"),
    # 저장소 전용 Python 해석기 탐색 회귀 test. 미러 독자는 이 test가 지키는 저장소 배치를
    # 갖지 않으므로 실행할 수도, 실행해서도 안 된다.
    ("samples", "cpp", "PythonResolution.Tests.ps1"),
    ("samples", "dotnet", "PythonResolution.Tests.ps1"),
    ("samples", "java", "PythonResolution.Tests.ps1"),
    # 저장소 전용 self-shell 경로 해석 회귀 test. 위와 같은 이유.
    ("samples", "cpp", "SelfShellResolution.Tests.ps1"),
    ("samples", "java", "SelfShellResolution.Tests.ps1"),
)

#  Windows 전용 script는 CRLF여야 한다. `.gitattributes`가 정한 것과 같다.
CRLF_SUFFIXES = (".bat", ".cmd", ".ps1")

#  풀었을 때 바로 실행할 수 있어야 하는 파일.
EXECUTABLE_NAMES = ("gradlew",)
EXECUTABLE_SUFFIXES = (".sh",)

TEXT_SUFFIXES = (
    ".bat", ".cmd", ".ps1", ".sh", ".cs", ".csproj", ".props", ".targets", ".sln",
    ".java", ".kt", ".kts", ".ts", ".js", ".json", ".toml", ".properties",
    ".cpp", ".hpp", ".h", ".txt", ".md", ".yml", ".yaml", ".xml", ".config",
    ".meta", ".unity", ".asset", ".gitignore",
)

LANGUAGE_REGION_RE = re.compile(r"^<!-- zlink-lang: (java|kotlin|end) -->\s*$")
README_LINK_RE = re.compile(r"(?<!!)\[[^\]]*\]\(([^)\s]+)")
# A README must never mention the other language's source directory. Keep this
# list explicit if a future code identifier genuinely needs that spelling.
OTHER_LANGUAGE_IDENTIFIER_EXCEPTIONS: frozenset[str] = frozenset()

#  미러 루트에 그대로 복사하는 저장소 루트 파일. 빌드 산출물 무시 규칙과 줄바꿈 규칙의
#  소유자는 이 둘이고, 미러에 두 번째 사본을 손으로 관리하지 않는다.
ROOT_FILES = (".gitignore", ".gitattributes")

#  Files under framework/languages/<source language>/ that a mirror needs at its root.
LANGUAGE_ROOT_FILES = {
    "kotlin": ("gradle/kotlin-detekt.gradle", "config/detekt/kotlin-java-terminals.yml"),
}

README_KO = """[English](./README.md) | **한국어**

# ZLink {title} examples

이 저장소는 [zlink-systems/zlink](https://github.com/zlink-systems/zlink)의
framework 예제 코드를 그대로 내보낸 읽기 전용 저장소다. 코드는
원본 저장소에서만 고치고, 그 결과가 이곳에 자동으로 실린다. `main`에는 가장 최근 릴리스와
그 뒤 원본에 합쳐진 수정이 들어 있고, 릴리스마다 framework 버전과 같은 이름의 태그
`vA.B.C`가 붙는다. 이슈와 pull request는 원본 저장소에 낸다. 이 저장소는 pull request를
받지 않는다.

| 디렉터리 | 내용 |
|---|---|
| [`quickstart/`](quickstart/) | 설치하고 첫 응답을 받아 보는 가장 짧은 예제. 기능은 다루지 않는다 |
| [`tutorial/`](tutorial/) | 기능을 하나씩 더해 가며 쌓아 올린 예제. 기능별 가이드가 이 코드를 인용한다 |
| [`samples/`](samples/) | 업무 흐름 하나를 처음부터 끝까지 구현한 application |

전제 조건과 빌드·실행·검증 절차는 각 디렉터리의 README에 있다. 가이드와 규격은
[zlink.systems](https://zlink.systems)에 있다.
"""

README_EN = """**English** | [한국어](./README.ko.md)

# ZLink {title} examples

A read-only mirror of framework examples in [zlink-systems/zlink](https://github.com/zlink-systems/zlink).
`main` is the latest release
plus the fixes merged since; each release is the tag `vA.B.C` (the framework version). Send
issues and pull requests to the source repository — this one accepts no PRs.

| Directory | Purpose |
|---|---|
| [`quickstart/`](quickstart/) | From install to the first reply. Adds no feature |
| [`tutorial/`](tutorial/) | Adds one feature at a time. The feature guides read this code |
| [`samples/`](samples/) | Applications that show a complete business flow |

Each directory's README carries the prerequisites, build, run and verify steps. The documentation
lives at [zlink.systems](https://zlink.systems).

"""


def run(args: list[str]) -> bytes:
    return subprocess.run(args, check=True, capture_output=True).stdout


def is_text(name: str) -> bool:
    p = pathlib.PurePosixPath(name)
    return p.suffix in TEXT_SUFFIXES or p.name in ("gradlew", "gradlew.bat", ".gitignore")


def normalize(name: str, data: bytes) -> bytes:
    """줄바꿈을 파일 종류에 맞춘다."""
    if not is_text(name):
        return data
    body = data.replace(b"\r\n", b"\n")
    if pathlib.PurePosixPath(name).suffix in CRLF_SUFFIXES:
        body = body.replace(b"\n", b"\r\n")
    return body


def executable(name: str) -> bool:
    p = pathlib.PurePosixPath(name)
    return p.name in EXECUTABLE_NAMES or p.suffix in EXECUTABLE_SUFFIXES


def excluded(section: str, lang: str, name: str) -> bool:
    # Kotlin sources live in the Java source tree, so its repository-only exclusions
    # are owned by the same source-language entry rather than copied into a second list.
    source_lang = "java" if lang == "kotlin" else lang
    return (section, source_lang, name) in EXCLUDED_ENTRIES


def write(target: pathlib.Path, data: bytes, mode: int) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    if os.name != "nt":
        target.chmod(mode)


def source_language(lang: str) -> str:
    return "java" if lang == "kotlin" else lang


def language_entry(name: str, lang: str) -> bool:
    """Whether a Java/Kotlin shared-project entry belongs in this language mirror."""
    parts = pathlib.PurePosixPath(name).parts
    if not parts or parts[0] not in ("java", "kotlin"):
        return True
    return parts[0] == lang


def filter_settings(data: bytes, lang: str) -> bytes:
    """Keep only one language's Gradle include entries in a shared settings file."""
    if lang not in ("java", "kotlin"):
        return data
    other = "kotlin" if lang == "java" else "java"
    kept = []
    for line in data.decode().splitlines(keepends=True):
        # Includes are the source of truth for exported subprojects. Match both
        # include("java:…") and include(":java:…") forms used by the three trees.
        if f'"{other}:' in line or f'":{other}:' in line:
            continue
        kept.append(line)
    return "".join(kept).encode()


def filter_readme(data: bytes, lang: str) -> bytes:
    """Keep the explicit language regions of a shared Java/Kotlin README."""
    if lang not in ("java", "kotlin"):
        return data
    region: str | None = None
    result = []
    for line in data.decode().splitlines(keepends=True):
        marker = LANGUAGE_REGION_RE.match(line.rstrip("\n"))
        if marker is None:
            if region in (None, lang):
                result.append(line)
            continue
        value = marker.group(1)
        if value == "end":
            if region is None:
                raise ValueError("zlink-lang end marker without an open region")
            region = None
        else:
            if region is not None:
                raise ValueError("nested zlink-lang region")
            region = value
    if region is not None:
        raise ValueError("unterminated zlink-lang region")
    return "".join(result).encode()


def externalize_readme_links(data: bytes, source: str, section: str, name: str, lang: str) -> bytes:
    """Keep exported README links local, or make monorepo-only links explicit GitHub links."""
    text = data.decode()

    def replace(match: re.Match[str]) -> str:
        destination = match.group(1)
        raw_path, separator, anchor = destination.partition("#")
        if not raw_path or raw_path.startswith(("https://", "http://", "mailto:")):
            return match.group(0)
        relative = posixpath.normpath(posixpath.join(section, posixpath.dirname(name), raw_path))
        if not relative.startswith("../") and relative != ".." and language_entry(relative, lang):
            return match.group(0)
        monorepo_path = posixpath.normpath(posixpath.join(source, posixpath.dirname(name), raw_path))
        url = "https://github.com/zlink-systems/zlink/blob/main/" + monorepo_path
        return match.group(0).replace(destination, url + (separator + anchor if separator else ""))

    return README_LINK_RE.sub(replace, text).encode()


def validate_exported_readmes(root: pathlib.Path, lang: str) -> None:
    """Reject escaped links and stale sibling-language directory references."""
    root = root.resolve()
    other = "kotlin" if lang == "java" else "java"
    errors = []
    for readme in root.rglob("README*.md"):
        text = readme.read_text(encoding="utf-8")
        for match in README_LINK_RE.finditer(text):
            destination = match.group(1).strip("<>")
            if destination.startswith(("#", "https://", "http://", "mailto:")):
                continue
            path = destination.split("#", 1)[0]
            if not path:
                continue
            target = (readme.parent / path).resolve()
            if not target.is_relative_to(root) or not target.exists():
                errors.append(f"{readme.relative_to(root)}: unresolved relative link {destination}")
        reference = f"{other}/"
        for number, line in enumerate(text.splitlines(), start=1):
            if reference in line and not any(item in line for item in OTHER_LANGUAGE_IDENTIFIER_EXCEPTIONS):
                errors.append(f"{readme.relative_to(root)}:{number}: sibling directory {reference}")
    if errors:
        raise ValueError("exported README validation failed:\n" + "\n".join(errors))


def export_section(lang: str, section: str, ref: str, root: pathlib.Path) -> int | None:
    src = "framework/languages/%s/%s" % (source_language(lang), section)
    listing = subprocess.run(
        ["git", "ls-tree", "-r", "--name-only", ref, src],
        capture_output=True, text=True)
    if listing.returncode != 0 or not listing.stdout.strip():
        return None

    #  `<ref>:<경로>` 꼴로 그 하위 트리만 지정한다. 경로를 pathspec으로 주면 저장소 안의
    #  깊은 경로가 그대로 따라와, 미러에 디렉터리가 네 겹 생긴다.
    raw = run(["git", "-c", "core.autocrlf=false", "archive", "--format=zip",
               "%s:%s" % (ref, src)])

    count = 0
    with zipfile.ZipFile(io.BytesIO(raw)) as source:
        for item in source.infolist():
            if item.is_dir() or not language_entry(item.filename, lang) or excluded(section, lang, item.filename):
                continue
            data = normalize(item.filename, source.read(item))
            if item.filename == "settings.gradle.kts":
                data = filter_settings(data, lang)
            elif pathlib.PurePosixPath(item.filename).name in ("README.md", "README.ko.md"):
                data = filter_readme(data, lang)
                data = externalize_readme_links(data, src, section, item.filename, lang)
            mode = 0o755 if executable(item.filename) else 0o644
            write(root / section / item.filename, data, mode)
            count += 1
    return count


def export_root_files(lang: str, ref: str, root: pathlib.Path) -> None:
    for name in ROOT_FILES:
        data = run(["git", "-c", "core.autocrlf=false", "show", "%s:%s" % (ref, name)])
        write(root / name, normalize(name, data), 0o644)
    #  Kotlin sections apply ../gradle/kotlin-detekt.gradle, which looks upward for the shared
    #  terminal configuration. Both live beside the sections in the repository, so the mirror
    #  keeps the same relative layout.
    for name in LANGUAGE_ROOT_FILES.get(lang, ()):
        src = "framework/languages/%s/%s" % (source_language(lang), name)
        data = run(["git", "-c", "core.autocrlf=false", "show", "%s:%s" % (ref, src)])
        write(root / name, normalize(name, data), 0o644)
    title = LANGUAGE_TITLES[lang]
    write(root / "README.ko.md", README_KO.format(title=title).encode(), 0o644)
    write(root / "README.md", README_EN.format(title=title).encode(), 0o644)


def export_engine(source_name: str, mirror_name: str, ref: str, out_dir: pathlib.Path) -> int | None:
    src = "framework/languages/engines/%s" % source_name
    listing = subprocess.run(
        ["git", "ls-tree", "-r", "--name-only", ref, src],
        capture_output=True, text=True)
    if listing.returncode != 0 or not listing.stdout.strip():
        return None

    root = out_dir / mirror_name
    if root.exists():
        shutil.rmtree(root)
    raw = run(["git", "-c", "core.autocrlf=false", "archive", "--format=zip",
               "%s:%s" % (ref, src)])

    count = 0
    with zipfile.ZipFile(io.BytesIO(raw)) as source:
        for item in source.infolist():
            if item.is_dir():
                continue
            data = normalize(item.filename, source.read(item))
            mode = 0o755 if executable(item.filename) else 0o644
            write(root / item.filename, data, mode)
            count += 1

    for name in ROOT_FILES:
        target = root / name
        if target.exists():
            continue
        data = run(["git", "-c", "core.autocrlf=false", "show", "%s:%s" % (ref, name)])
        write(target, normalize(name, data), 0o644)
    return count


def export(lang: str, ref: str, out_dir: pathlib.Path) -> dict[str, int | None]:
    root = out_dir / ("zlink-%s-examples" % lang)
    if root.exists():
        shutil.rmtree(root)
    counts = {section: export_section(lang, section, ref, root) for section in SECTIONS}
    export_root_files(lang, ref, root)
    validate_exported_readmes(root, lang)
    return counts


def main() -> int:
    out_dir = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".artifacts/examples")
    ref = sys.argv[2] if len(sys.argv) > 2 else "HEAD"

    os.chdir(run(["git", "rev-parse", "--show-toplevel"]).decode().strip())
    out_dir.mkdir(parents=True, exist_ok=True)

    print("%-8s %-11s %6s  %s" % ("언어", "절", "파일", "산출물"))
    missing = 0
    for lang in LANGUAGES:
        root = out_dir / ("zlink-%s-examples" % lang)
        for section, count in export(lang, ref, out_dir).items():
            if count is None:
                missing += 1
                print("%-8s %-11s %6s  (추적 파일 없음)" % (lang, section, "-"))
                continue
            print("%-8s %-11s %6d  %s" % (lang, section, count, root / section))

    for source_name, mirror_name in ENGINE_EXPORTS.items():
        count = export_engine(source_name, mirror_name, ref, out_dir)
        if count is None:
            print("%-8s %-11s %6s  (이 ref에는 engine tree 없음)" % ("engines", source_name, "-"))
            continue
        print("%-8s %-11s %6d  %s" % ("engines", source_name, count, out_dir / mirror_name))

    print()
    print("각 zlink-<언어>-examples/ 와 engine mapping의 산출물이 미러 저장소의 루트가 된다.")
    return 1 if missing else 0


if __name__ == "__main__":
    raise SystemExit(main())
