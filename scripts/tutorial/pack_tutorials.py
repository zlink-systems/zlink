#!/usr/bin/env python3
"""tutorial과 samples를 언어별 zip으로 묶는다.

독자가 하나를 돌려 보려고 저장소 전체를 clone할 이유가 없다. 실제 소스는 빌드 산출물을 뺀
뒤에는 작다.

`git archive`가 담을 파일을 정한다. 커밋된 파일만 들어가므로 `bin`·`obj`·`node_modules`·
`dist`를 제외하는 규칙을 따로 관리하지 않아도 된다. 그 규칙은 `.gitignore`가 이미 갖고 있다.

tutorial과 samples 모두 배포된 패키지만 참조하므로(NuGet · npm · CMake `find_package`) 받은
그대로 빌드된다. samples는 저장소를 못 찾으면 package mode로 넘어가고, 그때 쓰는 버전은
`sync-version`이 갱신한다. 릴리즈 시점에 묶으면 그 릴리즈의 버전이 이미 박혀 있다.

**`git archive`가 그대로 내지 못하는 것 둘을 여기서 바로잡는다.**

- **줄바꿈.** Windows에서 묶으면 `core.autocrlf`가 끼어들어 `gradlew` 같은 shell script까지
  CRLF가 되고, 그 zip을 WSL이나 Linux에서 풀면 `bad interpreter: /bin/sh^M`으로 죽는다.
  반대로 `.bat`·`.cmd`·`.ps1`은 CRLF여야 한다. `.gitattributes`가 정한 규칙을 zip에 적용한다.
- **실행 권한.** `git archive`가 zip에 POSIX mode를 싣지 않아, 풀면 `gradlew`와 `*.sh`가
  실행되지 않는다.

사용:
    python scripts/tutorial/pack_tutorials.py [출력_디렉터리] [git_ref]

기본값은 `.artifacts/tutorials`와 `HEAD`다.
"""
from __future__ import annotations

import io
import pathlib
import subprocess
import sys
import zipfile

#  Java와 Kotlin은 gradle 프로젝트 하나를 공유하므로 함께 묶는다. 둘을 나누면
#  `settings.gradle.kts`와 wrapper가 한쪽에만 들어가 다른 쪽이 빌드되지 않는다.
LANGUAGES = ("cpp", "dotnet", "java", "node")

#  묶는 절. 저장소 경로와 zip 이름의 가운데 토막이다.
SECTIONS = ("tutorial", "samples")

#  Windows 전용 script는 CRLF여야 한다. `.gitattributes`가 정한 것과 같다.
CRLF_SUFFIXES = (".bat", ".cmd", ".ps1")

#  풀었을 때 바로 실행할 수 있어야 하는 파일.
EXECUTABLE_NAMES = ("gradlew",)
EXECUTABLE_SUFFIXES = (".sh",)

TEXT_SUFFIXES = (
    ".bat", ".cmd", ".ps1", ".sh", ".cs", ".csproj", ".props", ".targets", ".sln",
    ".java", ".kt", ".kts", ".ts", ".js", ".json", ".toml", ".properties",
    ".cpp", ".hpp", ".h", ".txt", ".md", ".yml", ".yaml", ".xml", ".gitignore",
)


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


def pack(lang: str, section: str, ref: str,
         out_dir: pathlib.Path) -> tuple[int, int] | None:
    src = "framework/languages/%s/%s" % (lang, section)
    listing = subprocess.run(
        ["git", "ls-tree", "-r", "--name-only", ref, src],
        capture_output=True, text=True)
    if listing.returncode != 0 or not listing.stdout.strip():
        return None

    #  `<ref>:<경로>` 꼴로 그 하위 트리만 지정한다. 경로를 pathspec으로 주면 저장소 안의
    #  깊은 경로가 그대로 따라와, 푼 자리에 디렉터리가 네 겹 생긴다.
    raw = run(["git", "-c", "core.autocrlf=false", "archive", "--format=zip",
               "%s:%s" % (ref, src)])

    name = "zlink-%s-%s" % (section, lang)
    prefix = name + "/"
    target = out_dir / (name + ".zip")
    count = 0
    with zipfile.ZipFile(io.BytesIO(raw)) as source, \
            zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as dest:
        for item in source.infolist():
            if item.is_dir():
                continue
            data = normalize(item.filename, source.read(item))
            info = zipfile.ZipInfo(prefix + item.filename, date_time=item.date_time)
            info.compress_type = zipfile.ZIP_DEFLATED
            mode = 0o755 if executable(item.filename) else 0o644
            info.external_attr = mode << 16
            dest.writestr(info, data)
            count += 1
    return count, target.stat().st_size


def main() -> int:
    out_dir = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".artifacts/tutorials")
    ref = sys.argv[2] if len(sys.argv) > 2 else "HEAD"

    root = run(["git", "rev-parse", "--show-toplevel"]).decode().strip()
    import os
    os.chdir(root)
    out_dir.mkdir(parents=True, exist_ok=True)

    print("%-10s %-10s %9s %6s  %s" % ("절", "언어", "크기", "파일", "산출물"))
    made = 0
    for section in SECTIONS:
        for lang in LANGUAGES:
            result = pack(lang, section, ref, out_dir)
            name = "zlink-%s-%s.zip" % (section, lang)
            if result is None:
                print("%-10s %-10s %9s %6s  (추적 파일 없음)"
                      % (section, lang, "-", "-"))
                continue
            count, size = result
            print("%-10s %-10s %8.0fK %6d  %s"
                  % (section, lang, size / 1024, count, out_dir / name))
            made += 1

    print()
    print("압축을 풀면 zlink-<절>-<언어>/ 아래에 project가 그대로 나온다.")
    return 0 if made else 1


if __name__ == "__main__":
    raise SystemExit(main())
