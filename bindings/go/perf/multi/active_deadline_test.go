package main

import (
	"testing"
	"time"

	"zlink.systems/zlink/perf/internal/perfcommon"
)

func TestDealerDealerUsesReceiveDeadline(t *testing.T) {
	const size = 64
	stats := perfcommon.NewMultiStats()
	payload := perfcommon.PreparePayload(size)
	perfcommon.StampPayloadPhaseAt(payload, perfcommon.PhaseActive, 50)
	for _, observed := range []int64{99, 100, 101} {
		recordMultiReceive(stats, payload, size, observed, perfcommon.BenchmarkWindow{StopAtNs: 100}, false, 1)
	}
	if count := stats.Snapshot(time.Second, size).Count; count != 1 {
		t.Fatalf("count = %d, want only the pre-deadline message", count)
	}
}
