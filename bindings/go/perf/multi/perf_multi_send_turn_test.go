package main

import (
	"context"
	"runtime"
	"testing"
	"time"

	zlink "zlink.systems/zlink"
)

type testSendSubmission struct {
	result  zlink.SubmitResult
	release <-chan struct{}
}

func (s testSendSubmission) Result() zlink.SubmitResult { return s.result }
func (s testSendSubmission) Admitted(context.Context) error {
	if s.release != nil {
		<-s.release
	}
	return nil
}

func TestMultiSendTurnWaitsForSocketAdmission(t *testing.T) {
	coordinator := newMultiSendTurnCoordinator(2)
	release := []chan struct{}{make(chan struct{}), make(chan struct{})}
	started := make(chan int, 3)
	submit := func(index int) (zlink.SendSubmission, error) {
		started <- index
		return testSendSubmission{result: zlink.SubmitBackpressured, release: release[index]}, nil
	}
	stopAt := time.Now().Add(time.Second)

	if submitted, err := coordinator.submitRound(stopAt, submit); err != nil || submitted != 2 {
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
	if submitted, err := coordinator.submitRound(stopAt, submit); err != nil || submitted != 0 {
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
	if submitted, err := coordinator.submitRound(stopAt, submit); err != nil || submitted != 1 {
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
