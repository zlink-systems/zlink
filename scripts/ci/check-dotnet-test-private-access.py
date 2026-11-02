#!/usr/bin/env python3
"""Reject NonPublic access flags in .NET test source, ignoring comments and literals."""

from pathlib import Path
import re
import sys


REPOSITORY = Path(__file__).resolve().parents[2]
TEST_ROOT = REPOSITORY / "framework/languages/dotnet/tests"
IGNORED = {"bin", "obj", ".artifacts", ".git", "node_modules"}
LEXEMES = re.compile(
    r'//[^\n]*|/\*[\s\S]*?\*/|\$*"{3,}[\s\S]*?"{3,}'
    r'|(?:\$@|@\$|@|\$)?"(?:""|\\.|[^"\\])*"'
    r"|'(?:\\.|[^'\\])*'"
)
FORBIDDEN = re.compile(r"\bNonPublic\b")


def code_mask(source):
    """Hide comments and string/character literals without changing line offsets."""

    def blank(match):
        return "".join("\n" if character == "\n" else " " for character in match.group())

    return LEXEMES.sub(blank, source)


def scan(test_root):
    if not test_root.is_dir():
        raise ValueError(f".NET test source root does not exist: {test_root}")
    files = sorted(
        path
        for path in test_root.rglob("*.cs")
        if not any(part in IGNORED for part in path.relative_to(test_root).parts)
    )
    if not files:
        raise ValueError(f"No .NET test source files found: {test_root}")
    matches = []
    for path in files:
        source = path.read_text(encoding="utf-8")
        for match in FORBIDDEN.finditer(code_mask(source)):
            line = source.count("\n", 0, match.start()) + 1
            matches.append(f"{path.relative_to(test_root).as_posix()}:{line}")
    return len(files), matches


def self_test():
    positive = "class Probe { BindingFlags /* split */ . NonPublic; }"
    negative = (
        '// BindingFlags.NonPublic\n'
        'var text = "BindingFlags.NonPublic";\n'
        "var character = 'x'; /* BindingFlags.NonPublic */\n"
        '$@"BindingFlags.NonPublic";\n'
    )
    if len(list(FORBIDDEN.finditer(code_mask(positive)))) != 1:
        raise ValueError("Scanner self-test did not detect a code reference.")
    if list(FORBIDDEN.finditer(code_mask(negative))):
        raise ValueError("Scanner self-test treated a comment or literal as code.")


def main():
    try:
        self_test()
        files, matches = scan(TEST_ROOT)
        if matches:
            print(".NET test private access: FAIL", file=sys.stderr)
            for match in matches:
                print(match, file=sys.stderr)
            return 1
        print(f".NET test private access: PASS ({files} sources; scanner self-test passed)")
        return 0
    except (OSError, ValueError) as error:
        print(f".NET test private access: FAIL\n{error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
