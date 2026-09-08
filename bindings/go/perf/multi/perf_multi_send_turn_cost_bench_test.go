package main

import (
	"runtime"
	"testing"
	"time"
)

func BenchmarkMultiSendTurnCoordinatorImmediate(b *testing.B) {
	const sockets = 100
	coordinator := newMultiSendTurnCoordinator(sockets)
	stopAt := time.Now().Add(time.Hour)
	submit := func(int) error { return nil }
	b.ReportAllocs()
	b.ReportMetric(sockets, "msg/round")
	b.ResetTimer()
	for round := 0; round < b.N; round++ {
		if submitted := coordinator.submitRound(stopAt, submit); submitted != sockets {
			b.Fatalf("submitted = %d, want %d", submitted, sockets)
		}
		for coordinator.pending != 0 {
			_, err := coordinator.drainReady()
			if err != nil {
				b.Fatal(err)
			}
			runtime.Gosched()
		}
	}
}
