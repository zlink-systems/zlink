package main

import (
	"testing"
	"time"

	zlink "zlink.systems/zlink"
)

func BenchmarkMultiSendTurnCoordinatorImmediate(b *testing.B) {
	const sockets = 100
	coordinator := newMultiSendTurnCoordinator(sockets)
	stopAt := time.Now().Add(time.Hour)
	submit := func(int) (zlink.SendSubmission, error) {
		return testSendSubmission{result: zlink.SubmitOK}, nil
	}
	b.ReportAllocs()
	b.ReportMetric(sockets, "msg/round")
	b.ResetTimer()
	for round := 0; round < b.N; round++ {
		if submitted, err := coordinator.submitRound(stopAt, submit); err != nil || submitted != sockets {
			b.Fatalf("submitted = %d, want %d", submitted, sockets)
		}
	}
}
