package main

import (
	"testing"
	"time"
)

func TestSharedScheduleUsesEpochAndFloorsWait(t *testing.T) {
	epoch := time.Now()
	schedule := newMultiSchedule(epoch, 100, 2*time.Second)
	if schedule.rate != 40 || schedule.due(epoch.Add(-time.Nanosecond)) || !schedule.reserve().Equal(epoch) ||
		!schedule.reserve().Equal(epoch.Add(25*time.Millisecond)) || !schedule.next().Equal(epoch.Add(50*time.Millisecond)) {
		t.Fatal("sends must be scheduled from epoch at 80% of the active receive rate")
	}
	for _, entry := range []struct{ remaining, wait time.Duration }{
		{999 * time.Microsecond, 0},
		{1999 * time.Microsecond, time.Millisecond},
		{150 * time.Millisecond, 50 * time.Millisecond},
	} {
		if got := multiWait(epoch, epoch.Add(entry.remaining)); got != entry.wait {
			t.Fatalf("wait with %v remaining = %v, want %v", entry.remaining, got, entry.wait)
		}
	}
}
