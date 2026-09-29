//go:build linux

// SPDX-License-Identifier: MPL-2.0

package native

import (
	"context"
	"runtime"
	"testing"
	"time"

	"zlink.systems/zlink/internal/native/completiontest"
)

func TestSendEarlyWritableJoinsBeforeDrainReturns(t *testing.T) {
	ctx, err := NewContext()
	if err != nil {
		t.Fatal(err)
	}
	defer ctx.Close()
	socket, err := ctx.RouterSocket()
	if err != nil {
		t.Fatal(err)
	}
	defer socket.Close()
	poller, err := NewPoller()
	if err != nil {
		t.Fatal(err)
	}
	defer poller.Close()
	if err := poller.AddSocket(socket, PollCompletion, 1); err != nil {
		t.Fatal(err)
	}

	completiontest.Start(socket.raw())
	defer completiontest.Stop()
	completiontest.EarlySendWaitOnce()
	defer completiontest.ReleaseEarlySend()

	type submitResult struct {
		submission SendSubmission
		err        error
	}
	submitted := make(chan submitResult, 1)
	target := NewRoutingIDString("submit-target")
	go func() {
		submission, submitErr := socket.SendTo(target).Bytes([]byte("retained")).Submit(context.Background())
		submitted <- submitResult{submission, submitErr}
	}()
	waitUntilEarlyJoin(t, completiontest.EarlySendWaiting)

	drained := make(chan error, 1)
	go func() {
		_, drainErr := socket.completion.drain(true)
		drained <- drainErr
	}()
	waitUntilEarlyJoin(t, func() bool {
		owner := socket.completion
		owner.mu.Lock()
		defer owner.mu.Unlock()
		return len(owner.earlyWritable) == 1 && len(owner.entries) == 0
	})
	select {
	case err := <-drained:
		t.Fatalf("drain returned before the early WRITABLE joined: %v", err)
	default:
	}
	completiontest.ReleaseEarlySend()
	var result submitResult
	select {
	case result = <-submitted:
	case <-time.After(5 * time.Second):
		t.Fatal("submit did not publish its native wait token")
	}
	if result.err != nil {
		t.Fatal(result.err)
	}
	select {
	case err := <-drained:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("drain did not finish the joined WRITABLE retry")
	}
	if err := result.submission.Admitted(context.Background()); err != nil {
		t.Fatalf("Admitted() error = %v", err)
	}
	owner := socket.completion
	owner.mu.Lock()
	entries, early := len(owner.entries), len(owner.earlyWritable)
	owner.mu.Unlock()
	if entries != 0 || early != 0 {
		t.Fatalf("completion state after retry: entries=%d early=%d", entries, early)
	}
	if got := completiontest.Trace(); got != "WNS" {
		t.Fatalf("native call order = %q, want WNS", got)
	}
}

func waitUntilEarlyJoin(t *testing.T, ready func() bool) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if ready() {
			return
		}
		runtime.Gosched()
	}
	t.Fatal("early completion did not reach the expected phase")
}
