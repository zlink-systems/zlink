package perf

import (
	"os/exec"
	"strings"
	"testing"
)

func TestRunnerParsesLatencyDurationBeforeBuilding(t *testing.T) {
	// The conflicting build options stop the runner after parsing and before any build.
	for _, tc := range []struct {
		args     []string
		envValue string
		want     string
	}{
		{[]string{"--latency-duration", "3"}, "0", "mutually exclusive"},
		{[]string{"--latency-duration=009"}, "", "mutually exclusive"},
		{[]string{"--duration", "7"}, "", "mutually exclusive"},
		{nil, "3", "mutually exclusive"},
		{nil, "invalid", "must be a positive integer"},
		{[]string{"--latency-duration=0"}, "", "must be a positive integer"},
		{[]string{"--latency-duration="}, "", "must be a positive integer"},
		{[]string{"--latency-duration"}, "", "requires a value"},
	} {
		args := append([]string{"run_benchmarks_multi.sh", "--reuse-build", "--clean-build"}, tc.args...)
		cmd := exec.Command("bash", args...)
		cmd.Env = append(cmd.Environ(), "ZLINK_CORE_SOURCE=local", "PERF_MULTI_DURATION_SECONDS=5", "PERF_MULTI_LATENCY_DURATION_SECONDS="+tc.envValue, "PERF_MULTI_MONITOR_HWM_BYTES=4096000")
		output, err := cmd.CombinedOutput()
		if err == nil || !strings.Contains(string(output), tc.want) {
			t.Errorf("%v with env %q: wanted %q, got %v\n%s", tc.args, tc.envValue, tc.want, err, output)
		}
	}
}
