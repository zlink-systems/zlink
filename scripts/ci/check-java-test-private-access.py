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
BYPASS = re.compile(
    r"\bprivateLookupIn\s*\(|\bUnsafe\b|\b(?:sun\.misc|jdk\.internal\.misc)\.Unsafe\b"
)
ENUMERATED_FIELD_LOOP = re.compile(
    r"for\s*\(\s*(?:var|[\w.$<>?,\[\]]+)\s+(?P<variable>[A-Za-z_$][\w$]*)\s*:"
    r"[\s\S]*?getDeclaredFields\s*\(\s*\)\s*\)\s*"
    r"(?:\{(?P<body>[^{}]*)\}|(?P<statement>[^;{}]*;))"
)
DECLARED_CALL = re.compile(
    r'(?:(?P<class_type>[A-Za-z_$][\w.$]*)\s*\.\s*class|'
    r'Class\s*\.\s*forName\s*\(\s*"(?P<named_type>[\w.$]+)"\s*\))\s*\.\s*'
    r'(?P<method>getDeclaredField|getDeclaredMethod)\s*(?P<opening>\()'
)
DECLARED_MEMBER_CALL = re.compile(
    r"\b(?P<method>getDeclaredField|getDeclaredMethod)\s*(?P<opening>\()"
)
DECLARED_DYNAMIC_RECEIVER = re.compile(
    r"\b(?P<receiver>[A-Za-z_$][\w$]*)\s*\.\s*"
    r"(?P<method>getDeclaredField|getDeclaredMethod)\s*\("
)
DECLARED_GETCLASS_RECEIVER = re.compile(
    r"\.\s*getClass\s*\(\s*\)\s*\.\s*"
    r"getDeclared(?:Field|Method)\s*\("
)
DECLARED_CONSTRUCTOR = re.compile(r"\bgetDeclaredConstructor\s*\(")
CLASS_FOR_NAME = re.compile(r"\bClass\s*\.\s*forName\s*(?P<opening>\()")
REFLECTIVE_FIELD_VALUE = re.compile(
    r"\.(?:get|set)(?:Boolean|Byte|Char|Double|Float|Int|Long|Short)?\s*\("
)
REFLECTIVE_METHOD_CALL = re.compile(r"\.\s*invoke\s*\(")
TYPE_DECL = re.compile(r"\b(?:class|interface|record|enum)\s+([A-Za-z_$][\w$]*)[^{};]*\{")
PRIVATE_FIELD = re.compile(r"\bprivate\b[^;{}]*?\b([A-Za-z_$][\w$]*)\s*(?:=|;)")
PRIVATE_METHOD = re.compile(r"\bprivate\b[^;{}]*?\b([A-Za-z_$][\w$]*)\s*\(")
IGNORED = {"build", ".gradle", "node_modules", ".git", ".artifacts"}


def tokens(source):
    return [m.group() for m in LEXEMES.finditer(source) if not m.group().startswith(("//", "/*"))]


def code_mask(source):
    """Keep offsets while hiding comments and string/character literals."""
    return LEXEMES.sub(
        lambda m: "".join("\n" if character == "\n" else " " for character in m.group())
        if m.group().startswith(("//", "/*", '"', "'"))
        else m.group(),
        source,
    )


def first_argument_end(masked, opening):
    """Return the end of a declared lookup's first argument expression."""
    depth = 0
    for index in range(opening + 1, len(masked)):
        token = masked[index]
        if token == "(":
            depth += 1
        elif token == ")":
            if depth == 0:
                return index
            depth -= 1
        elif token == "," and depth == 0:
            return index
    return len(masked)


def matching_call_end(masked, opening):
    """Return the closing parenthesis for a call, including nested calls."""
    depth = 0
    for index in range(opening, len(masked)):
        if masked[index] == "(":
            depth += 1
        elif masked[index] == ")":
            depth -= 1
            if depth == 0:
                return index
    return len(masked)


def string_literals(source):
    """Return string literal values, excluding literals inside comments."""
    return [
        token[1:-1]
        for match in LEXEMES.finditer(source)
        if (token := match.group()).startswith('"')
    ]


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


def private_members(java_root):
    """Index declared private fields and methods by declaring type name."""
    members = {}
    for path in java_root.rglob("*.java"):
        relative = path.relative_to(java_root)
        if any(part in IGNORED for part in relative.parts):
            continue
        source = path.read_text(encoding="utf-8")
        masked = code_mask(source)
        for declaration in TYPE_DECL.finditer(masked):
            opening = masked.find("{", declaration.start(), declaration.end())
            depth = 0
            closing = None
            for index in range(opening, len(masked)):
                if masked[index] == "{":
                    depth += 1
                elif masked[index] == "}":
                    depth -= 1
                    if depth == 0:
                        closing = index
                        break
            if closing is None:
                continue
            body = masked[opening + 1 : closing]
            names = {match.group(1) for match in PRIVATE_FIELD.finditer(body)}
            names.update(match.group(1) for match in PRIVATE_METHOD.finditer(body))
            members.setdefault(declaration.group(1), set()).update(names)
    return members


def scan(java_root):
    if not java_root.is_dir():
        raise ValueError(f"Java source root does not exist: {java_root}")
    files = sorted(p for p in java_root.rglob("*.java") if is_test_source(p.relative_to(java_root)))
    if not files:
        raise ValueError(f"No Java test sources found: {java_root}")
    private_index = private_members(java_root)
    private_member_names = set().union(*private_index.values()) if private_index else set()
    sites = {}
    private_accesses = []
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
        for bypass in BYPASS.finditer(masked):
            line = masked.count("\n", 0, bypass.start()) + 1
            private_accesses.append(
                f"{path.relative_to(java_root).as_posix()}:{line} private-access bypass"
            )
        for enumeration in ENUMERATED_FIELD_LOOP.finditer(masked):
            variable = re.escape(enumeration.group("variable"))
            member_access = re.compile(
                r"\b" + variable + r"\s*\.\s*(?:get|set)"
                r"(?:Boolean|Byte|Char|Double|Float|Int|Long|Short)?\s*\("
            )
            body = enumeration.group("body") or enumeration.group("statement")
            if member_access.search(body):
                line = masked.count("\n", 0, enumeration.start()) + 1
                private_accesses.append(
                    f"{path.relative_to(java_root).as_posix()}:{line} "
                    "reads or writes a value through enumerated declared fields"
                )
        for lookup in DECLARED_CALL.finditer(source):
            # A matching call inside a comment or literal is not executable code.
            method = lookup.group("method")
            if masked[lookup.start("method") : lookup.end("method")] != method:
                continue
            type_name = (lookup.group("class_type") or lookup.group("named_type")).rsplit(
                ".", 1
            )[-1]
            opening = lookup.start("opening")
            argument_end = first_argument_end(masked, opening)
            expression = source[opening + 1 : argument_end]
            private_members_found = sorted(
                set(string_literals(expression)) & private_index.get(type_name, set())
            )
            if private_members_found:
                line = masked.count("\n", 0, lookup.start()) + 1
                members = ", ".join(f"{type_name}.{member}" for member in private_members_found)
                private_accesses.append(
                    f"{path.relative_to(java_root).as_posix()}:{line} "
                    f"{members} use declared reflection on a private member"
                )
        for lookup in DECLARED_CALL.finditer(masked):
            method = lookup.group("method")
            if method not in {"getDeclaredField", "getDeclaredMethod"}:
                continue
            opening = lookup.start("opening")
            closing = matching_call_end(masked, opening)
            suffix = masked[closing + 1 :]
            accessor = (
                REFLECTIVE_FIELD_VALUE
                if method == "getDeclaredField"
                else REFLECTIVE_METHOD_CALL
            )
            accessed = accessor.match(suffix.lstrip()) is not None
            if not accessed:
                boundary = max(
                    masked.rfind(";", 0, lookup.start()),
                    masked.rfind("{", 0, lookup.start()),
                    masked.rfind("}", 0, lookup.start()),
                ) + 1
                assignment = re.search(
                    r"(?:[\w.$<>?,\[\]]+\s+)?(?P<name>[A-Za-z_$][\w$]*)\s*=\s*$",
                    masked[boundary : lookup.start()],
                )
                if assignment:
                    name = re.escape(assignment.group("name"))
                    accessor_use = re.compile(
                        r"\b" + name + r"\s*" + accessor.pattern
                    )
                    accessed = accessor_use.search(masked[closing + 1 :]) is not None
            if accessed:
                line = masked.count("\n", 0, lookup.start()) + 1
                description = (
                    "reflectively reads or writes a declared field value"
                    if method == "getDeclaredField"
                    else "reflectively invokes a declared method"
                )
                private_accesses.append(
                    f"{path.relative_to(java_root).as_posix()}:{line} {description}"
                )
        for lookup in DECLARED_MEMBER_CALL.finditer(masked):
            method = lookup.group("method")
            if masked[lookup.start("method") : lookup.end("method")] != method:
                continue
            opening = lookup.start("opening")
            argument_end = first_argument_end(masked, opening)
            argument = source[opening + 1 : argument_end].strip()
            if not re.fullmatch(r'"(?:\\.|[^"\\])*"', argument):
                line = masked.count("\n", 0, lookup.start()) + 1
                private_accesses.append(
                    f"{path.relative_to(java_root).as_posix()}:{line} "
                    "uses a dynamic declared-reflection member name"
                )
        for lookup in DECLARED_DYNAMIC_RECEIVER.finditer(masked):
            if lookup.group("receiver") == "class":
                continue
            opening = lookup.end() - 1
            argument_end = first_argument_end(masked, opening)
            argument = source[opening + 1 : argument_end].strip()
            members = string_literals(argument)
            if len(members) == 1 and members[0] in private_member_names:
                line = masked.count("\n", 0, lookup.start()) + 1
                private_accesses.append(
                    f"{path.relative_to(java_root).as_posix()}:{line} "
                    "uses dynamic class reflection on a private member name"
                )
        for lookup in DECLARED_GETCLASS_RECEIVER.finditer(masked):
            opening = lookup.end() - 1
            argument_end = first_argument_end(masked, opening)
            argument = source[opening + 1 : argument_end].strip()
            members = string_literals(argument)
            if len(members) == 1 and members[0] in private_member_names:
                line = masked.count("\n", 0, lookup.start()) + 1
                private_accesses.append(
                    f"{path.relative_to(java_root).as_posix()}:{line} "
                    "uses getClass() reflection on a private member name"
                )
        for lookup in DECLARED_CONSTRUCTOR.finditer(masked):
            line = masked.count("\n", 0, lookup.start()) + 1
            private_accesses.append(
                f"{path.relative_to(java_root).as_posix()}:{line} "
                "uses declared reflection on a constructor"
            )
        for lookup in CLASS_FOR_NAME.finditer(masked):
            close = matching_call_end(masked, lookup.start("opening"))
            argument = source[lookup.end("opening") : close].strip()
            if re.fullmatch(r'"(?:\\.|[^"\\])*"', argument):
                continue
            chained = re.match(
                r"\s*\.\s*getDeclared(?:Field|Method|Constructor)\s*\(",
                masked[close + 1 :],
            )
            if chained:
                line = masked.count("\n", 0, lookup.start()) + 1
                private_accesses.append(
                    f"{path.relative_to(java_root).as_posix()}:{line} "
                    "uses a dynamic class name for declared reflection"
                )
        if counts:
            sites[path.relative_to(java_root).as_posix()] = dict(sorted(counts.items()))
    return sites, len(files), private_accesses


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
    actual, file_count, private_accesses = scan(java_root)
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
    errors.extend(private_accesses)
    if errors:
        raise ValueError("\n".join(errors))
    return (
        file_count,
        sum(sum(accesses.values()) for accesses in actual.values()),
        len(actual),
        len(private_accesses),
    )


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
        files, accesses, legacy_files, private_reflection = check(
            args.java_root, allowlist, baseline
        )
        print(
            "Java test private access: PASS "
            f"({files} sources, {accesses} elevated-access calls in {legacy_files} files, "
            f"{private_reflection} named private-reflection accesses)"
        )
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Java test private access: FAIL\n{error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
