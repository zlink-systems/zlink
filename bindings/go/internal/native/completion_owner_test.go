package native

import (
	"context"
	"errors"
	"syscall"
	"testing"
)

func TestSendCompletionErrorPreservesResult(t *testing.T) {
	tests := []struct {
		name          string
		sendResult    SendCompleteResult
		errno         int
		result        SubmitResult
		expectedErrno int
	}{
		{name: "route removed", sendResult: SendNotFound, errno: int(syscall.ENOENT), result: SubmitNotFound},
		{name: "stream disconnected", sendResult: SendNotConnected, errno: int(syscall.ENOTCONN), result: SubmitNotConnected},
		{name: "send timed out", sendResult: SendTimedOut, errno: int(syscall.EAGAIN), result: SubmitInternalError, expectedErrno: int(syscall.EPROTO)},
		{name: "unknown result", sendResult: SendCompleteResult(999), errno: int(syscall.ENOENT), result: SubmitInternalError, expectedErrno: int(syscall.EPROTO)},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			var submitErr *SubmitError
			err := sendCompletionError(test.sendResult, test.errno)
			expectedErrno := test.expectedErrno
			if expectedErrno == 0 {
				expectedErrno = test.errno
			}
			if !errors.As(err, &submitErr) || submitErr.Result != test.result || submitErr.InternalErrno() != expectedErrno {
				t.Fatalf("sendCompletionError(%d) = %v, want result %d with errno %d", test.sendResult, err, test.result, expectedErrno)
			}
		})
	}
}

func TestRequestCompletionEntryJoinsCaptureBeforePublish(t *testing.T) {
	entry := newCompletionEntry(completionRequest)
	entry.capture(nil, nil)
	select {
	case <-entry.done:
		t.Fatal("request capture must not settle before submit publishes its completion id")
	default:
	}
	entry.publish(42)
	if parts, err := entry.waitRequest(context.Background()); err != nil || len(parts) != 0 {
		t.Fatalf("waitRequest() = (%v, %v), want (empty, nil)", parts, err)
	}
	entry.waitSettled()
}

func TestCompletionEntryPreservesLateRequestAfterWaitCancellation(t *testing.T) {
	waitCtx, cancel := context.WithCancel(context.Background())
	entry := newCompletionEntry(completionRequest)
	cancel()
	if _, err := entry.waitRequest(waitCtx); !errors.Is(err, context.Canceled) {
		t.Fatalf("waitRequest() error = %v, want context.Canceled", err)
	}
	part, err := NewMessage([]byte("late"))
	if err != nil {
		t.Fatalf("NewMessage() error = %v", err)
	}
	entry.capture([]*Message{part}, nil)
	entry.publish(43)
	entry.waitSettled()
	parts, err := entry.waitRequest(context.Background())
	if err != nil || len(parts) != 1 || string(parts[0].Data()) != "late" {
		t.Fatalf("late request result = (%v, %v)", parts, err)
	}
	MultipartClose(parts)
}

func TestImmediateManagedSendAllocationBudget(t *testing.T) {
	ctx, err := NewContext()
	if err != nil {
		t.Fatal(err)
	}
	defer ctx.Close()
	sender, err := ctx.PairSocket()
	if err != nil {
		t.Fatal(err)
	}
	defer sender.Close()
	receiver, err := ctx.PairSocket()
	if err != nil {
		t.Fatal(err)
	}
	defer receiver.Close()
	if err = receiver.Bind("inproc://send-allocation-budget"); err != nil {
		t.Fatal(err)
	}
	if err = sender.Connect("inproc://send-allocation-budget"); err != nil {
		t.Fatal(err)
	}
	data := make([]byte, 64)
	exchange := func() {
		body, err := NewMessage(data)
		if err != nil {
			t.Fatal(err)
		}
		tail, err := NewMessageWithSize(0)
		if err != nil {
			t.Fatal(err)
		}
		if err = submitNativeSend(context.Background(), sender.Send().MoveMessage(body).Message(tail)); err != nil {
			t.Fatal(err)
		}
		var received Received
		if ok, err := receiver.Recv(&received, RecvFlagsNone); err != nil || !ok {
			t.Fatalf("recv=(%v,%v)", ok, err)
		}
		if !body.closed || !tail.closed {
			t.Fatal("admitted send did not consume its sources")
		}
		received.Close()
	}
	exchange()
	// Two public input messages, the builder, retained native packet and receive
	// wrappers are included. The pre-optimization path allocated 32 objects;
	// admitting an immediate send must not allocate a completion entry.
	if allocations := testing.AllocsPerRun(100, exchange); allocations > 17 {
		t.Fatalf("immediate two-part send/receive allocated %.0f objects, budget 17", allocations)
	}
}

func submitNativeSend(ctx context.Context, op SendSubmitOp) error {
	submission, err := op.Submit(ctx)
	if err != nil {
		return err
	}
	return submission.Admitted(ctx)
}
