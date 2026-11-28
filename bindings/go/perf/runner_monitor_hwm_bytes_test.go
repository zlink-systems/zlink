package perf

import (
	"os/exec"
	"strings"
	"testing"
)

func TestRunnerParsesMonitorHWMBytesBeforeBuilding(t *testing.T) {
	// The conflicting build options stop the runner after parsing and before any build.
	for _, tc := range []struct {
		args     []string
		envValue string
		want     string
	}{
		{[]string{"--monitor-hwm-bytes", "0"}, "invalid", "mutually exclusive"},
		{nil, "4096000", "mutually exclusive"},
		{nil, "invalid", "--monitor-hwm-bytes must be a non-negative integer"},
		{[]string{"--monitor-hwm-bytes", ""}, "", "--monitor-hwm-bytes must be a non-negative integer"},
		{[]string{"--monitor-hwm-bytes"}, "", "--monitor-hwm-bytes requires a value"},
	} {
		args := append([]string{"run_benchmarks_multi.sh", "--reuse-build", "--clean-build"}, tc.args...)
		cmd := exec.Command("bash", args...)
		cmd.Env = append(cmd.Environ(), "ZLINK_CORE_SOURCE=local", "PERF_MULTI_DURATION_SECONDS=5",
			"PERF_MULTI_LATENCY_DURATION_SECONDS=", "PERF_MONITOR_HWM_BYTES=", "PERF_MULTI_MONITOR_HWM_BYTES="+tc.envValue)
		output, err := cmd.CombinedOutput()
		if err == nil || !strings.Contains(string(output), tc.want) {
			t.Errorf("%v with env %q: wanted %q, got %v\n%s", tc.args, tc.envValue, tc.want, err, output)
		}
	}
}
