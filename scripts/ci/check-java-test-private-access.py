#!/usr/bin/env python3
"""Reject test reflection outside the decreasing #2309 migration allowlist."""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


REPOSITORY = Path(__file__).resolve().parents[2]
JAVA_ROOT = REPOSITORY / "framework/languages/java"
ALLOWLIST = JAVA_ROOT / "gradle/test-private-access-allowlist.json"
LEXEMES = re.compile(
    r'//[^\n]*|/\*[\s\S]*?\*/|"""[\s\S]*?"""|"(?:\\.|[^"\\])*"'
    r"|'(?:\\.|[^'\\])*'|[A-Za-z_$][\w$]*|\d+|[^\s]"
)
ACCESS = re.compile(r"\b(setAccessible|trySetAccessible)\s*\(")
IGNORED = {"build", ".gradle", "node_modules", ".git", ".artifacts"}


def tokens(source):
    return [m.group() for m in LEXEMES.finditer(source) if not m.group().startswith(("//", "/*"))]


def code_mask(source):
    """Keep offsets while hiding comments and string/character literals."""
    return LEXEMES.sub(
        lambda m: " " * len(m.group())
        if m.group().startswith(("//", "/*", '"', "'"))
        else m.group(),
        source,
    )


def is_test_source(path):
    parts = path.parts
    if any(part in IGNORED for part in parts):
        return False
    # Include helpers without 'Test' in their names, custom test source sets,
    # samples, perf tests, and templates. Production reflection is out of scope.
    return "Test" in path.name or any(
        part == "src" and parts[index + 1] != "main"
        for index, part in enumerate(parts[:-1])
    )


def scan(java_root):
    if not java_root.is_dir():
        raise ValueError(f"Java source root does not exist: {java_root}")
    files = sorted(p for p in java_root.rglob("*.java") if is_test_source(p.relative_to(java_root)))
    if not files:
        raise ValueError(f"No Java test sources found: {java_root}")
    sites = {}
    for path in files:
        source = path.read_text(encoding="utf-8")
        masked = code_mask(source)
        counts = Counter()
        for access in ACCESS.finditer(masked):
            receiver_match = re.search(r"\b([A-Za-z_$][\w$]*)\s*\.\s*$", masked[: access.start()])
            receiver = receiver_match.group(1) if receiver_match else None
            end = masked.find(")", access.end())
            if end < 0:
                raise ValueError(f"Unclosed reflection invocation: {path}")
            # Include the receiver's declaration, so changing the reflected member
            # does not inherit an exception just because the variable name matches.
            origins = list(
                re.finditer(r"\b" + re.escape(receiver) + r"\s*=\s*[^;]*;", masked[: access.start()])
            ) if receiver else []
            origin = source[origins[-1].start() : origins[-1].end()] if origins else ""
            start = receiver_match.start() if receiver_match else max(
                masked.rfind(";", 0, access.start()),
                masked.rfind("{", 0, access.start()),
                masked.rfind("}", 0, access.start()),
            ) + 1
            identity = json.dumps(
                [tokens(origin), tokens(source[start : end + 1])],
                ensure_ascii=False,
            )
            counts[hashlib.sha256(identity.encode("utf-8")).hexdigest()] += 1
        if counts:
            sites[path.relative_to(java_root).as_posix()] = dict(sorted(counts.items()))
    return sites, len(files)


def read_allowlist(text):
    entries = json.loads(text)
    if not isinstance(entries, dict):
        raise ValueError("Allowlist must be an object keyed by Java test source path")
    for path, entry in entries.items():
        if Path(path).is_absolute() or ".." in Path(path).parts:
            raise ValueError(f"Invalid allowlist path: {path}")
        if not isinstance(entry.get("reason"), str) or not entry["reason"].strip():
            raise ValueError(f"Missing migration reason: {path}")
        accesses = entry.get("accesses")
        if not isinstance(accesses, dict) or not accesses:
            raise ValueError(f"Empty access list: {path}")
        for identity, count in accesses.items():
            if not re.fullmatch(r"[0-9a-f]{64}", identity) or type(count) is not int or count < 1:
                raise ValueError(f"Invalid access identity/count: {path}")
    return entries


def check(java_root, allowlist, baseline=None):
    actual, file_count = scan(java_root)
    expected = {path: entry["accesses"] for path, entry in allowlist.items()}
    errors = []
    for path in sorted(actual.keys() | expected.keys()):
        found = Counter(actual.get(path, {}))
        allowed = Counter(expected.get(path, {}))
        if found - allowed:
            errors.append(f"Unlisted reflection: {path} ({sum((found - allowed).values())} accesses)")
        if allowed - found:
            errors.append(f"Stale allowance; remove migrated accesses: {path} ({sum((allowed - found).values())} accesses)")
    if baseline is not None:
        for path, accesses in expected.items():
            previous = baseline.get(path, {}).get("accesses", {})
            if Counter(accesses) - Counter(previous):
                errors.append(f"Allowlist may only shrink: {path}")
    if errors:
        raise ValueError("\n".join(errors))
    return file_count, sum(sum(accesses.values()) for accesses in actual.values()), len(actual)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java-root", type=Path, default=JAVA_ROOT)
    parser.add_argument("--allowlist", type=Path, default=ALLOWLIST)
    base = parser.add_mutually_exclusive_group()
    base.add_argument("--baseline", type=Path, help="Compare against a saved allowlist")
    base.add_argument("--base-ref", help="Compare against this local Git revision; no fetch is performed")
    args = parser.parse_args()
    try:
        allowlist = read_allowlist(args.allowlist.read_text(encoding="utf-8"))
        baseline = None
        if args.baseline:
            baseline = read_allowlist(args.baseline.read_text(encoding="utf-8"))
        if args.base_ref:
            subprocess.run(
                ["git", "rev-parse", "--verify", args.base_ref + "^{commit}"],
                cwd=REPOSITORY, check=True, capture_output=True,
            )
            relative = args.allowlist.resolve().relative_to(REPOSITORY).as_posix()
            exists = subprocess.check_output(
                ["git", "ls-tree", args.base_ref, "--", relative], cwd=REPOSITORY, text=True,
            )
            if exists:
                baseline = read_allowlist(subprocess.check_output(
                    ["git", "show", f"{args.base_ref}:{relative}"], cwd=REPOSITORY, text=True,
                ))
            else:
                print("Initial migration allowlist: absent from base revision")
        files, accesses, legacy_files = check(args.java_root, allowlist, baseline)
        print(f"Java test private access: PASS ({files} sources, {accesses} legacy accesses in {legacy_files} files)")
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Java test private access: FAIL\n{error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
