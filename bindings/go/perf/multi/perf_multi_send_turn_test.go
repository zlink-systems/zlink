package main

import (
	"runtime"
	"testing"
	"time"
)

func TestMultiSendTurnWaitsForSocketAdmission(t *testing.T) {
	coordinator := newMultiSendTurnCoordinator(2)
	release := []chan struct{}{make(chan struct{}), make(chan struct{})}
	started := make(chan int, 3)
	submit := func(index int) error {
		started <- index
		<-release[index]
		return nil
	}
	stopAt := time.Now().Add(time.Second)

	if coordinator.submitRound(stopAt, submit) != 2 {
		t.Fatal("first round submitted no sockets")
	}
	seen := map[int]bool{}
	for len(seen) < 2 {
		select {
		case index := <-started:
			seen[index] = true
		case <-time.After(time.Second):
			t.Fatal("first round did not start every socket")
		}
	}
	if coordinator.submitRound(stopAt, submit) != 0 {
		t.Fatal("pending sockets were submitted again before admission completed")
	}

	close(release[0])
	deadline := time.Now().Add(time.Second)
	for len(coordinator.completed) == 0 && time.Now().Before(deadline) {
		runtime.Gosched()
	}
	progressed, err := coordinator.drainReady()
	if err != nil || !progressed {
		t.Fatalf("drainReady() = (%v, %v), want (true, nil)", progressed, err)
	}
	if coordinator.submitRound(stopAt, submit) != 1 {
		t.Fatal("completed socket was not available in the next round")
	}
	select {
	case index := <-started:
		if index != 0 {
			t.Fatalf("next submitted socket = %d, want 0", index)
		}
	case <-time.After(time.Second):
		t.Fatal("next round did not submit the completed socket")
	}

	close(release[1])
}
