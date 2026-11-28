import pytest

import run_benchmarks as runner


@pytest.mark.parametrize(
    ("options", "environment", "expected"),
    [
        (["--latency-duration", "3"], "0", "3"),
        (["--latency-duration=009"], "", "009"),
        ([], "3", "3"),
        ([], "", "7"),
    ],
)
def test_runner_passes_latency_duration_to_measurement_env(
    monkeypatch, options, environment, expected
):
    class ParsedOptions(Exception):
        pass

    def check_environment(env):
        assert env["PERF_MULTI_DURATION_SECONDS"] == "7"
        assert env["PERF_MULTI_LATENCY_DURATION_SECONDS"] == expected
        raise ParsedOptions

    monkeypatch.setenv("PERF_MULTI_LATENCY_DURATION_SECONDS", environment)
    monkeypatch.setattr(runner, "require_binding_runtime", lambda _: None)
    monkeypatch.setattr(runner, "_configure_core_runtime", check_environment)
    with pytest.raises(ParsedOptions):
        runner.main(["--pattern", "DEALER_DEALER", "--duration", "7", *options])


@pytest.mark.parametrize(
    ("options", "environment", "error"),
    [
        (["--latency-duration=0"], "", "must be a positive integer"),
        (["--latency-duration=-1"], "", "must be a positive integer"),
        (["--latency-duration="], "", "must be a positive integer"),
        (["--latency-duration"], "", "expected one argument"),
        ([], "invalid", "must be a positive integer"),
    ],
)
def test_runner_rejects_invalid_latency_duration(
    monkeypatch, capsys, options, environment, error
):
    monkeypatch.setenv("PERF_MULTI_LATENCY_DURATION_SECONDS", environment)
    with pytest.raises(SystemExit):
        runner.parse_args(options)
    assert error in capsys.readouterr().err
