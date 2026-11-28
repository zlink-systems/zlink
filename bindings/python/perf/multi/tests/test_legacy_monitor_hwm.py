import pytest

import run_benchmarks as runner
from perf_multi_common import resolve_multi_monitor_hwm_bytes


@pytest.mark.parametrize("name", ["PERF_MULTI_MONITOR_HWM", "PERF_MONITOR_HWM"])
@pytest.mark.parametrize("value", ["1024", ""])
def test_monitor_hwm_resolver_rejects_legacy_environment(name, value):
    with pytest.raises(SystemExit, match=rf"{name} .*{name}_BYTES"):
        resolve_multi_monitor_hwm_bytes(
            {name: value, "PERF_MULTI_MONITOR_HWM_BYTES": "4096"}
        )


@pytest.mark.parametrize("name", ["PERF_MULTI_MONITOR_HWM", "PERF_MONITOR_HWM"])
@pytest.mark.parametrize("options", [[], ["--monitor-hwm-bytes", "4096"]])
def test_runner_rejects_legacy_monitor_hwm_before_running(monkeypatch, name, options):
    def unexpected_runtime(_):
        pytest.fail("Runner reached runtime configuration with a legacy HWM")

    monkeypatch.setenv(name, "1024")
    monkeypatch.setattr(runner, "require_binding_runtime", lambda _: None)
    monkeypatch.setattr(runner, "_configure_core_runtime", unexpected_runtime)

    with pytest.raises(SystemExit, match=rf"{name} .*{name}_BYTES"):
        runner.main(["--pattern", "DEALER_DEALER", *options])
