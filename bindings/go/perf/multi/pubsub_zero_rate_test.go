package main

import (
	"testing"
	"time"
)

func TestPubSubZeroRateCannotCatchUp(t *testing.T) {
	var tracker multiCatchUp
	tracker.observe(0, []uint64{0}, 0, 0)
	if tracker.observe(500*time.Millisecond, []uint64{0}, 0, 0) {
		t.Fatal("caught up without a positive target rate")
	}
}
