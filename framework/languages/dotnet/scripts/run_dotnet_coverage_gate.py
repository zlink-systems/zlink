#!/usr/bin/env python3
import argparse
import os
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--threshold", type=float, default=80.0)
    parser.add_argument("--test-timeout-seconds", type=int, default=300)
    parser.add_argument("--no-test", action="store_true")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    results_dir = root / "artifacts" / "coverage"

    if not args.no_test:
        shutil.rmtree(results_dir, ignore_errors=True)
        results_dir.mkdir(parents=True, exist_ok=True)
        for test_project in test_projects(root):
            project_results_dir = results_dir / test_project.stem
            project_results_dir.mkdir(parents=True, exist_ok=True)
            run(
                [
                    "dotnet",
                    "test",
                    str(test_project),
                    "/p:CollectCoverage=true",
                    "/p:CoverletOutputFormat=cobertura",
                    f"/p:CoverletOutput={project_results_dir / 'coverage'}",
                ],
                root,
                args.test_timeout_seconds,
            )

    covered, total, files = collect_coverage(root, results_dir)
    if total == 0:
        print("No .NET framework source lines were found in coverage reports.", file=sys.stderr)
        return 1

    percentage = covered * 100.0 / total
    print(
        f".NET framework source line coverage: {percentage:.2f}% "
        f"({covered}/{total}, {files} files)"
    )
    if percentage + 1e-9 < args.threshold:
        print(
            f".NET framework source line coverage {percentage:.2f}% "
            f"is below {args.threshold:.2f}%",
            file=sys.stderr,
        )
        return 1
    return 0


def test_projects(root: Path) -> list[Path]:
    tests_root = root / "tests"
    return [
        tests_root / "Zlink.Framework.UnitTests" / "Zlink.Framework.UnitTests.csproj",
        tests_root / "Zlink.Framework.ContractTests" / "Zlink.Framework.ContractTests.csproj",
        tests_root / "Systems.Zlink.Stream.Connector.Tests" / "Systems.Zlink.Stream.Connector.Tests.csproj",
        tests_root / "Zlink.HttpClient.UnitTests" / "Zlink.HttpClient.UnitTests.csproj",
    ]


def run(command: list[str], cwd: Path, timeout_seconds: int) -> None:
    try:
        result = subprocess.run(command, cwd=cwd, timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        print(
            f"Command timed out after {timeout_seconds}s: {' '.join(command)}",
            file=sys.stderr,
        )
        raise SystemExit(124)
    if result.returncode != 0:
        raise SystemExit(result.returncode)


def collect_coverage(root: Path, results_dir: Path) -> tuple[int, int, int]:
    src_root = root / "src"
    covered_lines: set[tuple[Path, int]] = set()
    executable_lines: set[tuple[Path, int]] = set()

    for report in results_dir.rglob("*.cobertura.xml"):
        tree = ET.parse(report)
        source_roots = [
            Path(source.text).resolve()
            for source in tree.findall(".//sources/source")
            if source.text
        ]
        source_roots.append(root)
        for class_node in tree.findall(".//class"):
            filename = class_node.attrib.get("filename")
            if not filename:
                continue
            source = resolve_source(Path(filename), source_roots)
            if not is_product_source(source, src_root):
                continue

            for line_node in class_node.findall("./lines/line"):
                number_text = line_node.attrib.get("number")
                hits_text = line_node.attrib.get("hits", "0")
                if not number_text:
                    continue
                key = (source, int(number_text))
                executable_lines.add(key)
                if int(hits_text) > 0:
                    covered_lines.add(key)

    return len(covered_lines), len(executable_lines), len({path for path, _ in executable_lines})


def resolve_source(filename: Path, source_roots: list[Path]) -> Path:
    if filename.is_absolute():
        return filename.resolve()
    for source_root in source_roots:
        candidate = (source_root / filename).resolve()
        if candidate.exists():
            return candidate
    return (source_roots[-1] / filename).resolve()


def is_product_source(source: Path, src_root: Path) -> bool:
    try:
        relative = source.relative_to(src_root)
    except ValueError:
        return False
    if source.suffix != ".cs":
        return False
    name = source.name
    if name.endswith(".g.cs") or name.endswith(".AssemblyInfo.cs"):
        return False
    return "bin" not in relative.parts and "obj" not in relative.parts


if __name__ == "__main__":
    raise SystemExit(main())
