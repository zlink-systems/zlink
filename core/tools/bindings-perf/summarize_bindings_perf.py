#!/usr/bin/env python3
"""Summarize current bindings perf state against core baselines."""

from __future__ import annotations

import argparse
import os
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Set, Tuple


ROOT_DIR = Path(__file__).resolve().parents[2]
DEFAULT_LANGUAGES = ["cpp", "dotnet", "java", "rust", "go", "node", "python"]
DEFAULT_TARGETS = {
    "cpp": "0.95",
    "dotnet": "0.90",
    "go": "0.85",
    "java": "0.90",
    "node": "0.75",
    "python": "0.75",
    "rust": "0.95",
}


@dataclass
class ReportSummary:
    path: Path
    comparable: bool
    reason: str
    worst_ratio: Optional[float] = None
    worst_key: Optional[Tuple[str, str, str]] = None
    comparable_rows: int = 0
    clients: Optional[int] = None
    recv_mode: Optional[str] = None
    skipped_latest: Optional[Path] = None
    skipped_reason: Optional[str] = None


@dataclass
class CompareSpec:
    recv_mode: str
    warmup_seconds: Optional[int]
    duration_seconds: Optional[int]
    options: Dict[str, str]
    patterns: Tuple[str, ...]
    transports: Tuple[str, ...]
    msg_sizes: Tuple[str, ...]


@dataclass
class SessionPerfHints:
    retained_reports: Dict[str, Path]
    rollback_reports: Set[Path]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--languages", default=os.getenv("BINDINGS_PERF_LANGUAGES", ""))
    parser.add_argument(
        "--baseline-dir",
        default=os.getenv(
            "BINDINGS_PERF_BASELINE_DIR",
            str(ROOT_DIR / "perf" / "baseline"),
        ),
    )
    parser.add_argument(
        "--baseline-recv-file",
        default=os.getenv("BINDINGS_PERF_BASELINE_RECV_FILE", ""),
    )
    parser.add_argument(
        "--baseline-callback-file",
        default=os.getenv("BINDINGS_PERF_BASELINE_CALLBACK_FILE", ""),
    )
    parser.add_argument(
        "--session-dir",
        default=os.getenv("SESSION_DIR_ENV", ""),
    )
    return parser.parse_args()


def selected_languages(raw: str) -> List[str]:
    if not raw.strip():
        return list(DEFAULT_LANGUAGES)
    return [token.strip() for token in raw.split(",") if token.strip()]


def resolve_baseline(baseline_dir: Path, explicit: str, mode: str) -> Path:
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            raise FileNotFoundError(f"baseline file not found: {path}")
        return path

    if mode == "recv":
        recv_candidates = sorted(baseline_dir.glob("perf_*recv*.txt"))
        if recv_candidates:
            return recv_candidates[-1]
        generic_candidates = sorted(
            path
            for path in baseline_dir.glob("perf_*.txt")
            if "callback" not in path.name
        )
        if generic_candidates:
            return generic_candidates[-1]
    else:
        callback_candidates = sorted(baseline_dir.glob("perf_*callback*.txt"))
        if callback_candidates:
            return callback_candidates[-1]

    raise FileNotFoundError(f"no {mode} baseline file in {baseline_dir}")


def parse_result_rows(path: Path) -> Dict[Tuple[str, str, str], float]:
    rows: Dict[Tuple[str, str, str], float] = {}
    with path.open("r", encoding="utf-8", errors="ignore") as handle:
        for line in handle:
            if not line.startswith("RESULT,"):
                continue
            parts = line.strip().split(",")
            if len(parts) < 7:
                continue
            if parts[1] != "current" or parts[5] != "throughput":
                continue
            try:
                rows[(parts[2], parts[3], parts[4])] = float(parts[6])
            except ValueError:
                continue
    return rows


def parse_effective_options(path: Path) -> Dict[str, str]:
    options: Dict[str, str] = {}
    capture = False
    with path.open("r", encoding="utf-8", errors="ignore") as handle:
        for line in handle:
            text = line.rstrip("\n")
            if text.startswith("## Effective Options (start)"):
                capture = True
                continue
            if text.startswith("## Effective Options (end)"):
                break
            if not capture:
                continue
            stripped = text.strip()
            if ":" not in stripped:
                continue
            key, value = stripped.split(":", 1)
            options[key.strip().lstrip("-").strip()] = value.strip()
    return options


def parse_option_int(options: Dict[str, str], *keys: str) -> Optional[int]:
    for key in keys:
        raw = options.get(key, "")
        digits = "".join(ch for ch in raw if ch.isdigit())
        if digits:
            return int(digits)
    return None


def parse_option_csv(options: Dict[str, str], key: str) -> Tuple[str, ...]:
    raw = options.get(key, "")
    if not raw:
        return ()
    return tuple(token.strip() for token in raw.split(",") if token.strip())


def expected_result_keys(
    baseline_rows: Dict[Tuple[str, str, str], float],
    compare_spec: CompareSpec,
) -> Optional[set[Tuple[str, str, str]]]:
    if not compare_spec.patterns or not compare_spec.transports or not compare_spec.msg_sizes:
        return None
    return {
        key
        for key in baseline_rows
        if key[0] in compare_spec.patterns
        and key[1] in compare_spec.transports
        and key[2] in compare_spec.msg_sizes
    }


def build_compare_spec(path: Path, mode: str) -> CompareSpec:
    options = parse_effective_options(path)
    recv_mode = options.get("recv_mode", mode).strip().lower()
    return CompareSpec(
        recv_mode=recv_mode,
        warmup_seconds=parse_option_int(options, "warmup_seconds", "warmup"),
        duration_seconds=parse_option_int(options, "duration_seconds", "duration"),
        options=options,
        patterns=parse_option_csv(options, "patterns"),
        transports=parse_option_csv(options, "transports"),
        msg_sizes=parse_option_csv(options, "msg_sizes"),
    )


def report_candidates(report_dir: Path, mode: str) -> List[Path]:
    if not report_dir.exists():
        return []
    reports = sorted(
        path
        for path in report_dir.glob("perf_*.txt")
        if (mode == "callback") == ("callback" in path.name)
    )
    reports.reverse()
    return reports


def parse_session_perf_hints(session_dir: Path) -> SessionPerfHints:
    retained_reports: Dict[str, Path] = {}
    rollback_reports: Set[Path] = set()
    path_pattern = re.compile(r"(/home/[^`\s]+perf_[^`\s]+\.txt)")

    def classify_text(text: str) -> None:
        current_mode: Optional[str] = None
        pending_rollback = False
        pending_retained = False
        for raw_line in text.splitlines():
            line = raw_line.strip()
            if not line:
                continue

            lowered = line.lower()
            if "recv comparable" in lowered or "multi recv" in lowered:
                current_mode = "recv"
            elif "callback comparable" in lowered or "multi callback" in lowered:
                current_mode = "callback"

            if (
                "rollback-only" in lowered
                or "rollback evidence" in lowered
                or "reverted one-shot seed experiment" in lowered
                or "do not treat it as current-workspace comparable" in lowered
            ):
                pending_rollback = True
            if (
                "latest valid official" in lowered
                or "retained recv official comparable" in lowered
                or "retained callback official comparable" in lowered
                or "retained recv comparable remains" in lowered
                or "retained callback comparable remains" in lowered
            ):
                pending_retained = True

            for match in path_pattern.finditer(raw_line):
                candidate = Path(match.group(1))
                path_mode = "callback" if "callback" in candidate.name else "recv"
                if pending_rollback:
                    rollback_reports.add(candidate)
                    pending_rollback = False
                    continue
                if pending_retained:
                    retained_reports[path_mode] = candidate
                    pending_retained = False
                    current_mode = path_mode
                    continue
                if current_mode in ("recv", "callback") and "retained" in lowered:
                    retained_reports[current_mode] = candidate

    for name in ("00_run_state.md", "00_handoff.md"):
        path = session_dir / name
        if path.is_file():
            classify_text(path.read_text(encoding="utf-8", errors="ignore"))

    return SessionPerfHints(retained_reports=retained_reports, rollback_reports=rollback_reports)


def expected_clients_for(pattern: str, options: Dict[str, str]) -> Optional[int]:
    raw = options.get("clients", "")
    if not raw:
        return None
    lowered = raw.lower()
    if "auto" in lowered:
        if pattern == "MULTI_STREAM":
            return parse_option_int(options, "default_stream_clients", "clients")
        return parse_option_int(options, "default_clients", "clients")
    digits = "".join(ch for ch in raw if ch.isdigit())
    if not digits:
        return None
    return int(digits)


def summarize_report(
    path: Path,
    baseline_rows: Dict[Tuple[str, str, str], float],
    compare_spec: CompareSpec,
) -> ReportSummary:
    options = parse_effective_options(path)
    rows = parse_result_rows(path)
    mode = "callback" if "callback" in path.name else "recv"

    if path.parent.parent.name == "single":
        return ReportSummary(path=path, comparable=False, reason="single baseline N/A", recv_mode=mode)

    report_recv_mode = options.get("recv", mode).strip().lower()
    report_warmup = parse_option_int(options, "warmup_seconds", "warmup")
    report_duration = parse_option_int(options, "duration_seconds", "duration")
    report_patterns = parse_option_csv(options, "patterns")
    report_transports = parse_option_csv(options, "transports")
    report_msg_sizes = parse_option_csv(options, "msg_sizes")
    if report_recv_mode != compare_spec.recv_mode:
        return ReportSummary(
            path=path,
            comparable=False,
            reason="latest report not comparable (recv mode mismatch)",
            recv_mode=report_recv_mode,
        )
    if (
        compare_spec.warmup_seconds is not None
        and compare_spec.duration_seconds is not None
        and (report_warmup != compare_spec.warmup_seconds or report_duration != compare_spec.duration_seconds)
    ):
        return ReportSummary(
            path=path,
            comparable=False,
            reason="latest report not comparable (warmup/duration mismatch)",
            recv_mode=report_recv_mode,
        )
    if report_patterns and set(report_patterns) != set(compare_spec.patterns):
        return ReportSummary(
            path=path,
            comparable=False,
            reason="latest report not comparable (pattern coverage mismatch)",
            recv_mode=report_recv_mode,
        )
    if report_transports and set(report_transports) != set(compare_spec.transports):
        return ReportSummary(
            path=path,
            comparable=False,
            reason="latest report not comparable (transport coverage mismatch)",
            recv_mode=report_recv_mode,
        )
    if report_msg_sizes and set(report_msg_sizes) != set(compare_spec.msg_sizes):
        return ReportSummary(
            path=path,
            comparable=False,
            reason="latest report not comparable (msg-size coverage mismatch)",
            recv_mode=report_recv_mode,
        )
    if not report_patterns or not report_transports or not report_msg_sizes:
        return ReportSummary(
            path=path,
            comparable=False,
            reason="latest report not comparable (missing effective coverage options)",
            recv_mode=report_recv_mode,
        )

    ratios: List[Tuple[float, Tuple[str, str, str]]] = []
    client_mismatch = False
    for key, binding_value in rows.items():
        baseline_value = baseline_rows.get(key)
        if baseline_value is None:
            continue
        expected_clients = expected_clients_for(key[0], options)
        baseline_expected_clients = expected_clients_for(key[0], compare_spec.options)
        if (
            baseline_expected_clients is not None
            and expected_clients is not None
            and expected_clients != baseline_expected_clients
        ):
            client_mismatch = True
            continue
        ratios.append((binding_value / baseline_value, key))

    raw_clients = options.get("clients", "")
    parsed_clients: Optional[int] = None
    if raw_clients and "auto" not in raw_clients.lower():
        digits = "".join(ch for ch in raw_clients if ch.isdigit())
        if digits:
            parsed_clients = int(digits)

    if not ratios:
        reason = "no comparable rows"
        if client_mismatch:
            reason = "latest report not comparable (client count mismatch)"
        return ReportSummary(
            path=path,
            comparable=False,
            reason=reason,
            clients=parsed_clients,
            recv_mode=report_recv_mode,
        )

    expected_keys = expected_result_keys(baseline_rows, compare_spec)
    if expected_keys is not None and set(rows.keys()) != expected_keys:
        return ReportSummary(
            path=path,
            comparable=False,
            reason="latest report not comparable (result coverage mismatch)",
            clients=parsed_clients,
            recv_mode=report_recv_mode,
        )

    ratios.sort(key=lambda item: item[0])
    worst_ratio, worst_key = ratios[0]
    return ReportSummary(
        path=path,
        comparable=True,
        reason="ok",
        worst_ratio=worst_ratio,
        worst_key=worst_key,
        comparable_rows=len(ratios),
        clients=parsed_clients,
        recv_mode=report_recv_mode,
    )


def latest_comparable_report(
    report_dir: Path,
    mode: str,
    baseline_rows: Dict[Tuple[str, str, str], float],
    compare_spec: CompareSpec,
    session_hints: Optional[SessionPerfHints] = None,
) -> Optional[ReportSummary]:
    reports = report_candidates(report_dir, mode)
    if not reports:
        return None

    if session_hints is not None:
        retained_path = session_hints.retained_reports.get(mode)
        if retained_path is not None and retained_path.is_file():
            summary = summarize_report(retained_path, baseline_rows, compare_spec)
            if summary.comparable:
                latest_summary = None
                for report in reports:
                    if report == retained_path or report in session_hints.rollback_reports:
                        continue
                    latest_summary = summarize_report(report, baseline_rows, compare_spec)
                    break
                if latest_summary is not None and latest_summary.path != summary.path:
                    summary.skipped_latest = latest_summary.path
                    summary.skipped_reason = latest_summary.reason
                return summary

        reports = [report for report in reports if report not in session_hints.rollback_reports]
        if not reports:
            return None

    latest_summary: Optional[ReportSummary] = None
    for report in reports:
        summary = summarize_report(report, baseline_rows, compare_spec)
        if latest_summary is None:
            latest_summary = summary
        if summary.comparable:
            if latest_summary is not None and latest_summary.path != summary.path:
                summary.skipped_latest = latest_summary.path
                summary.skipped_reason = latest_summary.reason
            return summary

    return latest_summary


def print_language_block(
    lang: str,
    target: str,
    recv_baseline: Dict[Tuple[str, str, str], float],
    callback_baseline: Dict[Tuple[str, str, str], float],
    recv_compare_spec: CompareSpec,
    callback_compare_spec: CompareSpec,
    session_hints: Optional[SessionPerfHints],
) -> List[Tuple[float, str, str, Tuple[str, str, str], Path]]:
    deficits: List[Tuple[float, str, str, Tuple[str, str, str], Path]] = []
    print(f"\n[{lang}] target={target}")
    runner = ROOT_DIR.parent / "bindings" / lang / "perf" / "run_benchmarks.sh"
    print(f"  runner: {'ok' if runner.is_file() and os.access(runner, os.X_OK) else 'missing'} {runner}")

    single_report = latest_comparable_report(
        ROOT_DIR.parent / "bindings" / lang / "perf" / "results" / "single" / "report",
        "callback",
        callback_baseline,
        callback_compare_spec,
        session_hints,
    )
    if single_report is None:
        print("  single: no callback report")
    else:
        suffix = ""
        if single_report.skipped_latest is not None and single_report.skipped_reason is not None:
            suffix = (
                f"; latest={single_report.skipped_latest.name}"
                f" skipped={single_report.skipped_reason}"
            )
        print(f"  single: {single_report.path.name} ({single_report.reason}{suffix})")

    for mode, baseline in (("recv", recv_baseline), ("callback", callback_baseline)):
        compare_spec = recv_compare_spec if mode == "recv" else callback_compare_spec
        summary = latest_comparable_report(
            ROOT_DIR.parent / "bindings" / lang / "perf" / "results" / "multi" / "report",
            mode,
            baseline,
            compare_spec,
            session_hints,
        )
        label = f"multi {mode}"
        if summary is None:
            print(f"  {label}: no report")
            continue
        if summary.comparable and summary.worst_ratio is not None and summary.worst_key is not None:
            prefix = ""
            if summary.skipped_latest is not None and summary.skipped_reason is not None:
                prefix = (
                    f"latest={summary.skipped_latest.name} "
                    f"skipped={summary.skipped_reason}; "
                )
            print(
                "  "
                f"{label}: {prefix}{summary.path.name} "
                f"worst={summary.worst_ratio:.3f} key={summary.worst_key} rows={summary.comparable_rows}"
            )
            deficits.append((summary.worst_ratio, lang, mode, summary.worst_key, summary.path))
        else:
            suffix = f", clients={summary.clients}" if summary.clients is not None else ""
            print(f"  {label}: {summary.path.name} ({summary.reason}{suffix})")
    return deficits


def main() -> int:
    args = parse_args()
    langs = selected_languages(args.languages)
    baseline_dir = Path(args.baseline_dir)
    recv_baseline_path = resolve_baseline(baseline_dir, args.baseline_recv_file, "recv")
    callback_baseline_path = resolve_baseline(
        baseline_dir, args.baseline_callback_file, "callback"
    )

    recv_baseline = parse_result_rows(recv_baseline_path)
    callback_baseline = parse_result_rows(callback_baseline_path)
    recv_compare_spec = build_compare_spec(recv_baseline_path, "recv")
    callback_compare_spec = build_compare_spec(callback_baseline_path, "callback")
    session_hints = None
    if args.session_dir:
        session_hints = parse_session_perf_hints(Path(args.session_dir))

    print("=== Bindings perf summary ===")
    print(f"Selected languages: {','.join(langs)}")
    print(f"Recv baseline: {recv_baseline_path}")
    print(f"Callback baseline: {callback_baseline_path}")
    print(
        "Targets: "
        + ", ".join(
            f"{lang}={os.getenv(f'BINDINGS_PERF_TARGET_{lang.upper()}', DEFAULT_TARGETS.get(lang, '0.80'))}"
            for lang in langs
        )
    )

    all_deficits: List[Tuple[float, str, str, Tuple[str, str, str], Path]] = []
    for lang in langs:
        target = os.getenv(f"BINDINGS_PERF_TARGET_{lang.upper()}", DEFAULT_TARGETS.get(lang, "0.80"))
        all_deficits.extend(
            print_language_block(
                lang,
                target,
                recv_baseline,
                callback_baseline,
                recv_compare_spec,
                callback_compare_spec,
                session_hints,
            )
        )

    if all_deficits:
        all_deficits.sort(key=lambda item: item[0])
        worst = all_deficits[0]
        print(
            "\nLargest current deficit: "
            f"{worst[1]} multi {worst[2]} ratio={worst[0]:.3f} "
            f"key={worst[3]} report={worst[4].name}"
        )
    else:
        print("\nLargest current deficit: no comparable multi report found yet")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
