//go:build linux

// SPDX-License-Identifier: MPL-2.0

package native

import (
	"context"
	"errors"
	"syscall"
	"testing"
	"time"

	"zlink.systems/zlink/internal/native/completiontest"
)

func TestTokenlessBackpressure(t *testing.T) {
	ctx := context.Background()
	target := NewRoutingIDString("submit-target")
	payload := []byte("payload")
	operations := []struct {
		name   string
		submit func(*RouterSocket) (SendSubmission, error)
	}{
		{"send", func(socket *RouterSocket) (SendSubmission, error) {
			return socket.SendTo(target).Bytes(payload).Submit(ctx)
		}},
		{"request", func(socket *RouterSocket) (SendSubmission, error) {
			return socket.Request(target).Bytes(payload).Submit(ctx)
		}},
	}
	for _, operation := range operations {
		t.Run(operation.name+"/initial", func(t *testing.T) {
			socket := newBackpressureTestSocket(t)
			completiontest.BackpressureOnce(false)
			submission, err := operation.submit(socket)
			if submission != nil {
				t.Fatalf("Submit() returned a submission without a wait token")
			}
			requireTokenlessBackpressure(t, err)
			checkBackpressureCleanup(t, socket.completion, "B")
		})
		t.Run(operation.name+"/retry", func(t *testing.T) {
			socket := newBackpressureTestSocket(t)
			completiontest.BackpressureOnce(true)
			submission, err := operation.submit(socket)
			if err != nil || submission == nil || submission.Result() != SubmitBackpressured {
				t.Fatalf("initial Submit() = (%v, %v), want backpressure with a wait token", submission, err)
			}

			completiontest.BackpressureOnce(false)
			if _, err := socket.completion.drain(true); err != nil {
				t.Fatal(err)
			}
			checkBackpressureCleanup(t, socket.completion, "BWNB")
			wait, cancel := context.WithTimeout(ctx, time.Second)
			defer cancel()
			requireTokenlessBackpressure(t, submission.Admitted(wait))
			if request, ok := submission.(RequestSubmission); ok {
				parts, err := request.Reply(wait)
				defer MultipartClose(parts)
				if len(parts) != 0 {
					t.Fatalf("Reply() returned %d parts after a failed admission", len(parts))
				}
				requireTokenlessBackpressure(t, err)
			}
		})
	}

	t.Run("reply/initial", func(t *testing.T) {
		socket := newBackpressureTestSocket(t)
		message, err := NewMessage(payload)
		if err != nil {
			t.Fatal(err)
		}
		defer message.Close()
		token := ReplyToken{owner: socket.replyOwner, value: 1}
		completiontest.BackpressureOnce(false)
		requireTokenlessBackpressure(t, socket.Reply(target, token).Message(message).Submit(ctx))
		checkBackpressureCleanup(t, socket.completion, "B")
	})
}

func newBackpressureTestSocket(t *testing.T) *RouterSocket {
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
	completiontest.Start(socket.raw())
	t.Cleanup(completiontest.Stop)
	return socket
}

func requireTokenlessBackpressure(t *testing.T, err error) {
	t.Helper()
	var submitErr *SubmitError
	if !errors.As(err, &submitErr) || submitErr.Result != SubmitBackpressured ||
		submitErr.InternalErrno() != int(syscall.EAGAIN) {
		t.Fatalf("submit error = %T %v, want SubmitError(Backpressured, EAGAIN)", err, err)
	}
}

func checkBackpressureCleanup(t *testing.T, owner *completionOwner, trace string) {
	t.Helper()
	owner.mu.Lock()
	defer owner.mu.Unlock()
	if len(owner.entries) != 0 || len(owner.earlyWritable) != 0 || owner.inflightSends != 0 {
		t.Fatalf("completion state: entries=%d early=%d in-flight=%d",
			len(owner.entries), len(owner.earlyWritable), owner.inflightSends)
	}
	if got := completiontest.Trace(); got != trace {
		t.Fatalf("native call trace = %q, want %q", got, trace)
	}
}
