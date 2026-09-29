//go:build linux

// SPDX-License-Identifier: MPL-2.0

package native

import (
	"errors"
	"syscall"
	"testing"

	"zlink.systems/zlink/internal/native/completiontest"
)

func TestWritableDeliversByContextAndToken(t *testing.T) {
	for _, kind := range []completionOperationKind{completionSendRetry, completionRequest} {
		for _, rid := range []string{"submit-target", "different-echo"} {
			t.Run(completionTestName(kind)+"/"+rid, func(t *testing.T) {
				owner, entry := newWritableFixture(t, kind)
				// RID echo belongs to Core. Injecting a different echo verifies that
				// the binding uses only its socket-local context and wait token.
				completiontest.Writable(41, entry.handleKey, rid)
				if _, err := owner.drain(true); err != nil {
					t.Fatal(err)
				}
				if _, err := owner.drain(true); err != nil {
					t.Fatal(err)
				}
				select {
				case <-entry.done:
					if entry.err != nil {
						t.Fatalf("WRITABLE waiter error = %v", entry.err)
					}
				default:
					t.Fatal("matching context/token did not complete the waiter")
				}
				if len(owner.entries) != 0 || len(writableFixturePayload(entry).owned) != 0 {
					t.Fatal("completed waiter retained its entry or payload")
				}
			})
		}
	}
}

func TestWritableProjectionForSendAndRequest(t *testing.T) {
	cases := []struct {
		name          string
		sendResult    int
		terminalErrno int
		result        SubmitResult
		errno         int
	}{
		{"not found", int(SendNotFound), int(syscall.ENOENT), SubmitNotFound, int(syscall.ENOENT)},
		{"not connected", int(SendNotConnected), int(syscall.ENOTCONN), SubmitNotConnected, int(syscall.ENOTCONN)},
		{"unknown", 999, int(syscall.ENOENT), SubmitInternalError, int(syscall.EPROTO)},
	}
	for _, kind := range []completionOperationKind{completionSendRetry, completionRequest} {
		for _, test := range cases {
			t.Run(completionTestName(kind)+"/"+test.name, func(t *testing.T) {
				owner, entry := newWritableFixture(t, kind)
				completiontest.WritableResult(41, entry.handleKey, "submit-target",
					test.sendResult, test.terminalErrno)
				if _, err := owner.drain(true); err != nil {
					t.Fatal(err)
				}
				if _, err := owner.drain(true); err != nil {
					t.Fatal(err)
				}
				select {
				case <-entry.done:
				default:
					t.Fatal("WRITABLE result did not settle the waiter")
				}
				var submitErr *SubmitError
				if !errors.As(entry.err, &submitErr) {
					t.Fatalf("WRITABLE result error = %T %v, want SubmitError", entry.err, entry.err)
				}
				if submitErr.Result != test.result || submitErr.InternalErrno() != test.errno {
					t.Fatalf("WRITABLE result = (%d, %d), want (%d, %d)", submitErr.Result, submitErr.InternalErrno(), test.result, test.errno)
				}
			})
		}
	}
}

func completionTestName(kind completionOperationKind) string {
	if kind == completionRequest {
		return "request"
	}
	return "send"
}

func writableFixturePayload(entry *completionEntry) *sendRetryPayload {
	if entry.kind == completionRequest {
		return entry.request.payload
	}
	return entry.send.payload
}

func newWritableFixture(t *testing.T, kind completionOperationKind) (*completionOwner, *completionEntry) {
	t.Helper()
	ctx, err := NewContext()
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = ctx.Close() })
	socket, err := ctx.RouterSocket()
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = socket.Close() })
	poller, err := NewPoller()
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = poller.Close() })
	if err := poller.AddSocket(socket, PollCompletion, 1); err != nil {
		t.Fatal(err)
	}
	owner := socket.completion
	entry := newCompletionEntry(kind)
	target := NewRoutingIDString("submit-target")
	parts := []sendBuilderPart{{bytes: true, data: []byte("retained")}}
	if kind == completionRequest {
		entry.request, err = newRequestRetryState(socket.socketCore, &target, 1000, parts)
	} else {
		var send sendRetryState
		send, err = newSendRetryState(socket.socketCore, &target, parts)
		entry.send = &send
	}
	if err != nil {
		t.Fatal(err)
	}
	if err := owner.register(entry); err != nil {
		t.Fatal(err)
	}
	entry.publishSendWait(41)
	if err := entry.setWritableWaiting(true); err != nil {
		t.Fatal(err)
	}
	completiontest.Start(socket.raw())
	t.Cleanup(completiontest.Stop)
	return owner, entry
}
