package main

import (
	"bytes"
	"runtime"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/perf/internal/perfcommon"
)

func TestMultiReqRepAdmissionWorkerRetriesSamePayloadBeforeAdmission(t *testing.T) {
	msgSize := perfcommon.MetricHeaderSize
	completion := make(chan zlink.RequestReplyCompletion, 1)
	events := make(chan multiReqRepEvent)
	completions := make(chan (<-chan zlink.RequestReplyCompletion))
	stopAdmissions := make(chan struct{})
	done := make(chan struct{})
	defer close(done)

	var mu sync.Mutex
	var attempts [][]byte
	submit := func(candidate []byte, _ time.Duration) (<-chan zlink.RequestReplyCompletion, error) {
		mu.Lock()
		attempts = append(attempts, append([]byte(nil), candidate...))
		attempt := len(attempts)
		mu.Unlock()
		if attempt == 1 {
			return nil, &zlink.SubmitError{Result: zlink.SubmitBackpressured}
		}
		if attempt > 2 {
			<-stopAdmissions
			return nil, &zlink.SubmitError{Result: zlink.SubmitBackpressured}
		}
		return completion, nil
	}

	now := time.Now()
	window := perfcommon.BenchmarkWindow{
		ActiveAt: now,
		StopAt:   now.Add(time.Second),
	}
	go runMultiReqRepAdmissionWorker(
		0, submit, msgSize, time.Second, window, completions,
		events, stopAdmissions, done)

	select {
	case event := <-events:
		if event.kind != multiReqRepAdmitted {
			t.Fatalf("first event kind = %d, want admitted", event.kind)
		}
	case <-time.After(time.Second):
		t.Fatal("request was not admitted")
	}
	select {
	case admittedCompletion := <-completions:
		if admittedCompletion != completion {
			t.Fatal("admission worker forwarded the wrong completion")
		}
	case <-time.After(time.Second):
		t.Fatal("admission worker did not forward the completion")
	}
	close(stopAdmissions)

	mu.Lock()
	if len(attempts) != 2 {
		mu.Unlock()
		t.Fatalf("submit attempts = %d, want 2", len(attempts))
	}
	if !bytes.Equal(attempts[0], attempts[1]) || len(attempts[0]) != msgSize {
		mu.Unlock()
		t.Fatalf("pre-admission retry changed payload: first=%x second=%x", attempts[0], attempts[1])
	}
	mu.Unlock()

	select {
	case event := <-events:
		if event.kind != multiReqRepAdmissionStopped {
			t.Fatalf("second event kind = %d, want admission stopped", event.kind)
		}
	case <-time.After(time.Second):
		t.Fatal("admission worker did not stop")
	}
}

func TestMultiReqRepSchedulerOverlapsAdmissionBeforePriorReply(t *testing.T) {
	firstCompletion := make(chan zlink.RequestReplyCompletion, 1)
	secondCompletion := make(chan zlink.RequestReplyCompletion, 1)
	thirdAdmission := make(chan struct{})
	secondStarted := make(chan struct{})
	var calls atomic.Int32

	submit := func(_ []byte, _ time.Duration) (<-chan zlink.RequestReplyCompletion, error) {
		switch calls.Add(1) {
		case 1:
			return firstCompletion, nil
		case 2:
			close(secondStarted)
			return secondCompletion, nil
		default:
			<-thirdAdmission
			return nil, &zlink.SubmitError{Result: zlink.SubmitBackpressured}
		}
	}

	now := time.Now()
	window := perfcommon.BenchmarkWindow{ActiveAt: now, StopAt: now.Add(100 * time.Millisecond)}
	done := make(chan error, 1)
	go func() {
		done <- runMultiReqRepScheduler(
			[]multiReqRepSubmit{submit}, perfcommon.NewMultiStats(),
			perfcommon.MetricHeaderSize, window, 200*time.Millisecond, time.Second)
	}()

	select {
	case <-secondStarted:
		// The second admission started while the first request still had no reply.
	case <-time.After(time.Second):
		t.Fatal("second request did not overlap the first request")
	}
	firstCompletion <- zlink.RequestReplyCompletion{Result: zlink.RequestTimedOut}
	secondCompletion <- zlink.RequestReplyCompletion{Result: zlink.RequestTimedOut}

	if wait := time.Until(window.StopAt); wait > 0 {
		<-time.After(wait)
	}
	close(thirdAdmission)
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("scheduler failed: %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("scheduler did not finish its bounded drain")
	}
}

func TestMultiReqRepSchedulerBoundsGoroutinesWhileRepliesArePending(t *testing.T) {
	const pendingTarget = 128
	var mu sync.Mutex
	completions := make([]chan zlink.RequestReplyCompletion, 0, pendingTarget)
	admitted := make(chan struct{})
	releaseAdmission := make(chan struct{})
	var calls atomic.Int32

	submit := func(_ []byte, _ time.Duration) (<-chan zlink.RequestReplyCompletion, error) {
		call := calls.Add(1)
		if call > pendingTarget {
			<-releaseAdmission
			return nil, &zlink.SubmitError{Result: zlink.SubmitBackpressured}
		}
		completion := make(chan zlink.RequestReplyCompletion, 1)
		mu.Lock()
		completions = append(completions, completion)
		if len(completions) == pendingTarget {
			close(admitted)
		}
		mu.Unlock()
		return completion, nil
	}

	baseline := runtime.NumGoroutine()
	now := time.Now()
	window := perfcommon.BenchmarkWindow{
		ActiveAt: now,
		StopAt:   now.Add(500 * time.Millisecond),
	}
	done := make(chan error, 1)
	go func() {
		done <- runMultiReqRepScheduler(
			[]multiReqRepSubmit{submit}, perfcommon.NewMultiStats(),
			perfcommon.MetricHeaderSize, window, 200*time.Millisecond, time.Second)
	}()

	select {
	case <-admitted:
	case <-time.After(time.Second):
		t.Fatal("scheduler did not build the pending reply set")
	}
	runtime.Gosched()
	if delta := runtime.NumGoroutine() - baseline; delta > 32 {
		t.Fatalf("pending replies added %d goroutines, want bounded worker count", delta)
	}

	if wait := time.Until(window.StopAt); wait > 0 {
		time.Sleep(wait)
	}
	close(releaseAdmission)
	mu.Lock()
	for _, completion := range completions {
		completion <- zlink.RequestReplyCompletion{Result: zlink.RequestTimedOut}
	}
	mu.Unlock()

	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("scheduler failed: %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("scheduler did not finish its bounded drain")
	}
}
