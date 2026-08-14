package perfcommon

import "testing"

func TestMonitorHwmBytesFromEnv(t *testing.T) {
	t.Run("default", func(t *testing.T) {
		t.Setenv("PERF_MONITOR_HWM_BYTES", "")
		t.Setenv("PERF_MULTI_MONITOR_HWM_BYTES", "")
		got, err := monitorHwmBytesFromEnv()
		if err != nil || got != 4_096_000 {
			t.Fatalf("monitorHwmBytesFromEnv() = (%d, %v), want (4096000, nil)", got, err)
		}
	})

	t.Run("common exact bytes", func(t *testing.T) {
		t.Setenv("PERF_MONITOR_HWM_BYTES", "8192")
		t.Setenv("PERF_MULTI_MONITOR_HWM_BYTES", "")
		got, err := monitorHwmBytesFromEnv()
		if err != nil || got != 8192 {
			t.Fatalf("monitorHwmBytesFromEnv() = (%d, %v), want (8192, nil)", got, err)
		}
	})

	t.Run("multi exact zero", func(t *testing.T) {
		t.Setenv("PERF_MONITOR_HWM_BYTES", "8192")
		t.Setenv("PERF_MULTI_MONITOR_HWM_BYTES", "0")
		got, err := monitorHwmBytesFromEnv()
		if err != nil || got != 0 {
			t.Fatalf("monitorHwmBytesFromEnv() = (%d, %v), want (0, nil)", got, err)
		}
	})

	t.Run("legacy env ignored", func(t *testing.T) {
		t.Setenv("PERF_MONITOR_HWM_BYTES", "")
		t.Setenv("PERF_MULTI_MONITOR_HWM_BYTES", "")
		t.Setenv("PERF_MULTI_MONITOR_HWM", "7")
		got, err := monitorHwmBytesFromEnv()
		if err != nil || got != 4_096_000 {
			t.Fatalf("monitorHwmBytesFromEnv() = (%d, %v), want byte default", got, err)
		}
	})
}
