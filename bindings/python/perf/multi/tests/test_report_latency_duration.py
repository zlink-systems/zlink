import pytest

import perf_report
import run_benchmarks as runner


@pytest.mark.parametrize(
    ("duration", "environment", "expected"),
    [
        ("7", None, "7"),
        ("7", "", "7"),
        ("7", "009", "009"),
        ("0", "3", "3"),
        ("0", None, None),
        ("1.5", "", None),
        ("7", "invalid", None),
    ],
)
def test_multi_report_uses_runner_latency_duration(
    monkeypatch, tmp_path, duration, environment, expected
):
    if environment is None:
        monkeypatch.delenv("PERF_MULTI_LATENCY_DURATION_SECONDS", raising=False)
    else:
        monkeypatch.setenv("PERF_MULTI_LATENCY_DURATION_SECONDS", environment)

    metrics = tmp_path / "metrics.csv"
    cases = tmp_path / "cases.csv"
    report = tmp_path / "report.txt"
    metrics.touch()
    cases.touch()
    options = [
        "render-multi",
        "--metrics", str(metrics),
        "--cases", str(cases),
        "--report", str(report),
        "--patterns", "DEALER_DEALER",
        "--transports", "tcp",
        "--msg-sizes", "64",
        "--runs", "1",
        "--duration", duration,
        "--lang", "python",
        "--clients", "1",
        "--sndtimeo-ms", "200",
        "--rcvtimeo-ms", "200",
        "--transport-transition-ms", "0",
        "--pattern-transition-ms", "0",
    ]

    if expected is None:
        with pytest.raises(SystemExit):
            runner.parse_args(["--duration", duration])
        with pytest.raises(SystemExit, match="must be a positive integer"):
            perf_report.main(options)
        assert not report.exists()
    else:
        assert runner.parse_args(["--duration", duration]).latency_duration == expected
        with pytest.raises(SystemExit) as result:
            perf_report.main(options)
        assert result.value.code == 0
        assert report.read_text().splitlines().count(
            f"- latency_duration_seconds: {expected}"
        ) == 2
