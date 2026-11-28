//go:build linux

// SPDX-License-Identifier: MPL-2.0

package native

import (
	"context"
	"errors"
	"syscall"
	"testing"

	"zlink.systems/zlink/internal/native/completiontest"
)

func TestInvalidWaitTokenErrno(t *testing.T) {
	for _, kind := range []completionOperationKind{completionSendRetry, completionRequest} {
		t.Run(completionTestName(kind)+"/initial", func(t *testing.T) {
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
			message, err := NewMessageString("caller payload")
			if err != nil {
				t.Fatal(err)
			}
			defer message.Close()

			completiontest.Start(socket.raw())
			defer completiontest.Stop()
			completiontest.InvalidWaitTokenOnce()
			target := NewRoutingIDString("submit-target")
			if kind == completionRequest {
				_, err = socket.Request(target).Message(message).Submit(context.Background())
			} else {
				_, err = socket.SendTo(target).Message(message).Submit(context.Background())
			}
			assertInvalidWaitTokenErrno(t, err)

			if got := string(message.Data()); got != "caller payload" {
				t.Fatalf("caller message = %q", got)
			}
			owner := socket.completion
			owner.mu.Lock()
			entries, inFlight := len(owner.entries), len(owner.inflightSends)
			owner.mu.Unlock()
			if entries != 0 || inFlight != 0 || completiontest.Trace() != "B" {
				t.Fatalf("failed submit left entries=%d, in-flight sends=%d, trace=%q",
					entries, inFlight, completiontest.Trace())
			}
		})

		t.Run(completionTestName(kind)+"/retry", func(t *testing.T) {
			owner, entry := newWritableFixture(t, kind)
			completiontest.InvalidWaitTokenOnce()
			completiontest.Writable(41, entry.handleKey, "submit-target")
			if _, err := owner.drain(true); err != nil {
				t.Fatal(err)
			}
			select {
			case <-entry.done:
				assertInvalidWaitTokenErrno(t, entry.err)
			default:
				t.Fatal("invalid retry token left the waiter pending")
			}
			if len(owner.entries) != 0 || len(writableFixturePayload(entry).owned) != 0 {
				t.Fatal("failed retry retained the entry or staged payload")
			}
		})
	}
}

func assertInvalidWaitTokenErrno(t *testing.T, err error) {
	t.Helper()
	var submitErr *SubmitError
	if !errors.As(err, &submitErr) || submitErr.Result != SubmitInternalError ||
		submitErr.InternalErrno() != int(syscall.EPROTO) {
		t.Fatalf("submit error = %v, want INTERNAL_ERROR/EPROTO", err)
	}
}
