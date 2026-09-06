//go:build linux

// SPDX-License-Identifier: MPL-2.0

package native

import (
	"context"
	"errors"
	"runtime"
	"testing"

	"zlink.systems/zlink/internal/native/completiontest"
)

func TestWritableResubmitsOnlyAfterNoData(t *testing.T) {
	for _, kind := range []completionOperationKind{completionSendRetry, completionRequest} {
		t.Run(completionTestName(kind), func(t *testing.T) {
			owner, entry := newWritableFixture(t, kind)
			second := newCompletionEntry(completionRequest, context.Background())
			if err := owner.register(second); err != nil {
				t.Fatal(err)
			}
			second.publish(42)
			completiontest.Writable(41, entry.handleKey, "submit-target")
			completiontest.Request(42, second.handleKey)
			drained, err := owner.drain(true)
			if err != nil {
				t.Fatal(err)
			}
			want := "WQNS"
			if kind == completionRequest {
				want = "WQNR"
			}
			if got := completiontest.Trace(); got != want {
				t.Fatalf("native call order = %q, want %q (W: WRITABLE, Q: REQUEST, N: NO_DATA, S/R: resubmit)", got, want)
			}
			if drained.processed != 2 || drained.requestCompletions != 1 {
				t.Fatalf("first drain = %+v, want two original completions and one request", drained)
			}
			if !second.settled || second.err != nil {
				t.Fatalf("second queued completion was not delivered: settled=%v err=%v", second.settled, second.err)
			}
			if kind == completionRequest {
				select {
				case <-entry.done:
					t.Fatal("retry's new completion was consumed by the original drain")
				default:
				}
				drained, err = owner.drain(true)
				if err != nil || drained.processed != 1 || drained.requestCompletions != 1 {
					t.Fatalf("next drain = (%+v, %v), want the retry's one REQUEST completion", drained, err)
				}
			}
			if !entry.settled || entry.err != nil || len(owner.entries) != 0 {
				t.Fatalf("retry did not settle cleanly: settled=%v err=%v entries=%d", entry.settled, entry.err, len(owner.entries))
			}
		})
	}
}

func TestWritableRetryCancellationDuringDrain(t *testing.T) {
	for _, kind := range []completionOperationKind{completionSendRetry, completionRequest} {
		t.Run(completionTestName(kind), func(t *testing.T) {
			owner, entry := newWritableFixture(t, kind)
			second := newCompletionEntry(completionRequest, context.Background())
			if err := owner.register(second); err != nil {
				t.Fatal(err)
			}
			// Hold the second completion at its submit/capture join, so caller
			// cancellation occurs after WRITABLE capture and before NO_DATA.
			defer second.publish(42)
			completiontest.Writable(41, entry.handleKey, "submit-target")
			completiontest.Request(42, second.handleKey)
			done := make(chan error, 1)
			go func() { _, err := owner.drain(true); done <- err }()
			captured := false
			for i := 0; i < 100_000; i++ {
				second.mu.Lock()
				captured = second.captured
				second.mu.Unlock()
				if captured {
					break
				}
				runtime.Gosched()
			}
			if !captured {
				t.Fatal("drain did not reach the second queued completion")
			}
			entry.cancel(context.Canceled)
			second.publish(42)
			if err := <-done; err != nil {
				t.Fatal(err)
			}
			if got := completiontest.Trace(); got != "WQN" {
				t.Fatalf("canceled retry made a native submission: trace=%q", got)
			}
			if !entry.settled || !errors.Is(entry.err, context.Canceled) || len(owner.entries) != 0 {
				t.Fatalf("canceled waiter cleanup: settled=%v err=%v entries=%d", entry.settled, entry.err, len(owner.entries))
			}
			if len(writableFixturePayload(entry).owned) != 0 {
				t.Fatal("canceled waiter retained its payload")
			}
		})
	}
}
