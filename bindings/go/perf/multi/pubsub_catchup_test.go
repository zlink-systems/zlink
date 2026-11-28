package main

import (
	"testing"
	"time"
)

func TestPubSubUsesCatchUpWindow(t *testing.T) {
	for _, observation := range []struct {
		increment uint64
		caughtUp  bool
	}{{9, true}, {8, false}} {
		var tracker multiCatchUp
		if tracker.observe(0, []uint64{92, 100}, 100) {
			t.Fatal("caught up before the window elapsed")
		}
		if got := tracker.observe(500*time.Millisecond, []uint64{102, 100 + observation.increment}, 110); got != observation.caughtUp {
			t.Fatalf("caught up with second subscriber delta %d = %v", observation.increment, got)
		}
	}

	var tracker multiCatchUp
	if tracker.observe(0, []uint64{92, 92}, 100) ||
		tracker.observe(100*time.Millisecond, []uint64{93, 93}, 102) ||
		tracker.observe(500*time.Millisecond, []uint64{102, 102}, 110) ||
		!tracker.observe(600*time.Millisecond, []uint64{104, 104}, 112) {
		t.Fatal("in-flight growth must restart the observation window")
	}
}
