package perfcommon

import (
	"os"
	"testing"
	"time"
)

func TestPercentileUsesLinearInterpolation(t *testing.T) {
	values := []float64{0, 10}
	if got := percentile(values, 95); got != 9.5 {
		t.Fatalf("percentile(95) = %v, want 9.5", got)
	}
	if got := percentile(values, 99); got != 9.9 {
		t.Fatalf("percentile(99) = %v, want 9.9", got)
	}
}

func TestRecordBytesRTTLatencySeparatesCountFromClockSample(t *testing.T) {
	stats := newStats(8)
	payload := make([]byte, MetricHeaderSize)
	now := time.Now()
	StampPayloadPhaseAt(payload, PhaseActive, now.Add(time.Hour))

	RecordBytesRTTLatency(
		stats, now.Add(-time.Second), now.Add(2*time.Hour), len(payload), payload)

	if got := stats.count; got != 1 {
		t.Fatalf("throughput count = %d, want 1", got)
	}
	if got := stats.latencyCount; got != 0 {
		t.Fatalf("latency sample count = %d, want 0", got)
	}
	if result := stats.Snapshot(time.Second, len(payload)); result.Valid {
		t.Fatal("result with no latency sample must be invalid")
	}
}

func TestRecordBytesRTTLatencyExcludesPostDeadlineMessage(t *testing.T) {
	stats := newStats(8)
	payload := make([]byte, MetricHeaderSize)
	now := time.Now()
	StampPayloadPhaseAt(payload, PhaseActive, now.Add(-time.Second))

	RecordBytesRTTLatency(
		stats, now.Add(-2*time.Second), now.Add(-time.Second), len(payload), payload)

	if got := stats.count; got != 0 {
		t.Fatalf("post-deadline throughput count = %d, want 0", got)
	}
	if got := stats.latencyCount; got != 0 {
		t.Fatalf("post-deadline latency sample count = %d, want 0", got)
	}
}

func TestLoadMultiConfigCanonicalizesSendSendAliases(t *testing.T) {
	tests := map[string]string{
		"MULTI_DEALER_ROUTER":          "MULTI_DEALER_ROUTER_SENDSEND",
		"DEALER_ROUTER_SENDSEND":       "MULTI_DEALER_ROUTER_SENDSEND",
		"MULTI_ROUTER_ROUTER":          "MULTI_ROUTER_ROUTER_SENDSEND",
		"MULTI_ROUTER_ROUTER_SENDSEND": "MULTI_ROUTER_ROUTER_SENDSEND",
	}
	for input, want := range tests {
		t.Run(input, func(t *testing.T) {
			got := LoadMultiConfig(input, "tcp", MetricHeaderSize, 1, 1).Pattern
			if got != want {
				t.Fatalf("canonical pattern = %q, want %q", got, want)
			}
		})
	}
}

func TestNewMultiStatsHonorsZeroLatencySampleCap(t *testing.T) {
	const envName = "PERF_MULTI_LATENCY_SAMPLE_CAP"
	previous, hadPrevious := os.LookupEnv(envName)
	t.Cleanup(func() {
		if hadPrevious {
			_ = os.Setenv(envName, previous)
		} else {
			_ = os.Unsetenv(envName)
		}
	})
	if err := os.Setenv(envName, "0"); err != nil {
		t.Fatal(err)
	}

	stats := NewMultiStats()
	stats.AddCount()
	stats.AddLatencySampleNs(123)

	if got := len(stats.latNs); got != 0 {
		t.Fatalf("retained latency samples = %d, want 0", got)
	}
	result := stats.Snapshot(time.Second, MetricHeaderSize)
	if !result.Valid || result.LatencyNs != 123 {
		t.Fatalf("zero-cap exact result = %+v, want valid mean 123", result)
	}
}

func TestNewStatsHonorsZeroLatencySampleCap(t *testing.T) {
	t.Setenv("PERF_SINGLE_LATENCY_SAMPLE_CAP", "0")

	stats := NewStats()
	stats.AddLatencyNs(123)

	if got := len(stats.latNs); got != 0 {
		t.Fatalf("retained latency samples = %d, want 0", got)
	}
	result := stats.Snapshot(time.Second, MetricHeaderSize)
	if !result.Valid || result.LatencyNs != 123 || result.LatencyP95Ns != 123 || result.LatencyP99Ns != 123 {
		t.Fatalf("zero-cap exact result = %+v, want valid mean/percentiles 123", result)
	}
}

func TestFinalizeResultUsesRoundTripBandwidthForMultiReqRep(t *testing.T) {
	for _, pattern := range []string{
		"MULTI_DEALER_ROUTER_REQREP",
		"MULTI_ROUTER_ROUTER_REQREP",
	} {
		t.Run(pattern, func(t *testing.T) {
			result := FinalizeResult(pattern, 1024, Result{Throughput: 1000})
			if result.Bandwidth != 2.048 {
				t.Fatalf("bandwidth = %v, want 2.048", result.Bandwidth)
			}
		})
	}
}
