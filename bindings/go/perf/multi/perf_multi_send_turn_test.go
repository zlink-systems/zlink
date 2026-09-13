package main

import (
	"context"
	"testing"
	"time"

	zlink "zlink.systems/zlink"
)

type testSendSubmission struct {
	result  zlink.SubmitResult
	release <-chan struct{}
}

func (s testSendSubmission) Result() zlink.SubmitResult { return s.result }
func (s testSendSubmission) Admitted(ctx context.Context) error {
	if s.release == nil {
		return nil
	}
	select {
	case <-s.release:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
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
	progressed, err := coordinator.resumeReady()
	if err != nil || !progressed {
		t.Fatalf("resumeReady() = (%v, %v), want (true, nil)", progressed, err)
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

func TestMultiSendTurnDoesNotWaitForImmediateAdmission(t *testing.T) {
	coordinator := newMultiSendTurnCoordinator(1)
	admitted := false
	submit := func(int) (zlink.SendSubmission, error) {
		return immediateTestSendSubmission{admitted: &admitted}, nil
	}

	if submitted, err := coordinator.submitRound(time.Now().Add(time.Second), submit); err != nil || submitted != 1 {
		t.Fatalf("submitRound() = (%d, %v), want (1, nil)", submitted, err)
	}
	if admitted {
		t.Fatal("immediately admitted send called Admitted")
	}
}

type immediateTestSendSubmission struct{ admitted *bool }

func (s immediateTestSendSubmission) Result() zlink.SubmitResult { return zlink.SubmitOK }
func (s immediateTestSendSubmission) Admitted(context.Context) error {
	*s.admitted = true
	return nil
}
