import csv


def emit_effective_options(lines, section, lang, suite, option_items):
    lines.append(f"## Effective Options ({section})")
    lines.append(f"- lang: {lang}")
    lines.append(f"- suite: {suite}")
    for key, value in option_items:
        lines.append(f"- {key}: {value}")
    lines.append("")


def emit_completion(lines, status, expected_result_lines, actual_result_lines):
    lines.append("## Completion")
    lines.append(f"- status: {status}")
    lines.append(f"- expected_result_lines: {expected_result_lines}")
    lines.append(f"- actual_result_lines: {actual_result_lines}")
    lines.append("")


def emit_failures(lines, failures):
    lines.append("## Failures")
    for pattern, transport, size, run, reason in failures:
        del run
        lines.append(f"- {pattern} current {transport} {size}B: {reason}")


def load_failures(path):
    failures = []
    with open(path, newline="", encoding="utf-8") as handle:
        reader = csv.reader(handle)
        for row in reader:
            if len(row) != 5:
                continue
            failures.append(tuple(row))
    return failures


def write_report(lines, report_path, output_path=None):
    text = "\n".join(lines) + "\n"
    with open(report_path, "w", encoding="utf-8") as report_file:
        report_file.write(text)
    if output_path:
        with open(output_path, "a", encoding="utf-8") as output_file:
            output_file.write(text)
    return text
