//go:build linux

// SPDX-License-Identifier: MPL-2.0

package native

import (
	"context"
	"testing"
	"time"

	"zlink.systems/zlink/internal/native/completiontest"
)

func TestLateInvalidWaitToken(t *testing.T) {
	for _, kind := range []completionOperationKind{completionSendRetry, completionRequest} {
		for _, recordKind := range []string{"writable", "completion"} {
			t.Run(completionTestName(kind)+"/"+recordKind, func(t *testing.T) {
				testLateInvalidWaitToken(t, kind, recordKind == "writable")
			})
		}
	}
}

func testLateInvalidWaitToken(t *testing.T, kind completionOperationKind, writable bool) {
	socket := newBackpressureTestSocket(t)
	owner := socket.completion
	target := NewRoutingIDString("submit-target")
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	completiontest.InvalidWaitTokenOnce()
	var err error
	if kind == completionRequest {
		_, err = socket.Request(target).Bytes([]byte("rejected")).Submit(ctx)
	} else {
		_, err = socket.SendTo(target).Bytes([]byte("rejected")).Submit(ctx)
	}
	assertInvalidWaitTokenErrno(t, err)
	stale := completiontest.InvalidWaitContext()
	if stale == 0 {
		t.Fatal("fixture did not record rejected context")
	}
	if writable {
		completiontest.Writable(41, stale, "submit-target")
	} else {
		completiontest.Request(41, stale)
	}

	// The next SEND is held inside Core with a legitimate early WRITABLE.
	// Its record has the same token number but a different context.
	completiontest.EarlySendWaitOnce()
	defer completiontest.ReleaseEarlySend()
	type result struct {
		submission SendSubmission
		err        error
	}
	submitted := make(chan result, 1)
	go func() {
		submission, err := socket.SendTo(target).Bytes([]byte("next")).Submit(ctx)
		submitted <- result{submission, err}
	}()
	waitUntilEarlyJoin(t, completiontest.EarlySendWaiting)

	drained := make(chan error, 1)
	go func() {
		progress, err := owner.drain(true)
		if err == nil && progress.processed != 2 {
			t.Errorf("drained %d records, want 2", progress.processed)
		}
		drained <- err
	}()
	var bufferedStale bool
	waitUntilEarlyJoin(t, func() bool {
		owner.mu.Lock()
		defer owner.mu.Unlock()
		_, bufferedStale = owner.inflightSends[stale]
		for key, record := range owner.inflightSends {
			if key != stale && record != nil {
				return true
			}
		}
		return false
	})
	completiontest.ReleaseEarlySend()

	select {
	case result := <-submitted:
		if result.err != nil {
			t.Fatal(result.err)
		}
		if err := result.submission.Admitted(ctx); err != nil {
			t.Fatal(err)
		}
	case <-ctx.Done():
		t.Fatal("next SEND did not publish")
	}
	select {
	case err := <-drained:
		if err != nil {
			t.Fatal(err)
		}
	case <-ctx.Done():
		t.Fatal("drain did not finish")
	}
	if bufferedStale {
		t.Error("late record was buffered while the next SEND was in flight")
	}
	trace := "BQWNS"
	if writable {
		trace = "BWWNS"
	}
	checkBackpressureCleanup(t, owner, trace)
}
