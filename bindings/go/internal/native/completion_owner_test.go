package native

import (
	"context"
	"errors"
	"syscall"
	"testing"
)

func TestImmediateSendRegistrationDoesNotStartRuntimeDrain(t *testing.T) {
	owner := newCompletionOwner(nil)
	entry := newSendCompletionEntry(context.Background(), nil, nextCompletionContext())
	if err := owner.register(entry); err != nil {
		t.Fatalf("register(send) error = %v", err)
	}
	if owner.runtime != nil {
		t.Fatal("immediate send registration started a completion poller before backpressure")
	}
	owner.unregister(entry)
}

func TestSendTerminalErrorPreservesCauseCategory(t *testing.T) {
	tests := []struct {
		name   string
		errno  int
		result SubmitResult
	}{
		{name: "route removed", errno: int(syscall.ENOENT), result: SubmitNotFound},
		{name: "socket shutdown", errno: int(syscall.ESHUTDOWN), result: SubmitTerminated},
		{name: "context terminated", errno: contextTerminatedErrno, result: SubmitTerminated},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			var submitErr *SubmitError
			err := sendTerminalError(test.errno)
			if !errors.As(err, &submitErr) || submitErr.Result != test.result || submitErr.InternalErrno() != test.errno {
				t.Fatalf("sendTerminalError(%d) = %v, want result %d with original errno", test.errno, err, test.result)
			}
		})
	}
}

func TestRequestTerminalErrorPreservesCauseCategory(t *testing.T) {
	tests := []struct {
		name   string
		errno  int
		result RequestResult
	}{
		{name: "route removed", errno: int(syscall.ENOENT), result: RequestNotFound},
		{name: "socket shutdown", errno: int(syscall.ESHUTDOWN), result: RequestTerminated},
		{name: "context terminated", errno: contextTerminatedErrno, result: RequestTerminated},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			var requestErr *RequestError
			err := requestTerminalError(test.errno)
			if !errors.As(err, &requestErr) || requestErr.Result != test.result || requestErr.InternalErrno() != test.errno {
				t.Fatalf("requestTerminalError(%d) = %v, want result %d with original errno", test.errno, err, test.result)
			}
		})
	}
}

func TestRequestCompletionEntryJoinsCaptureBeforePublish(t *testing.T) {
	entry := newCompletionEntry(completionRequest, context.Background())
	entry.capture(nil, nil)
	select {
	case <-entry.done:
		t.Fatal("request capture must not settle before submit publishes its completion id")
	default:
	}
	entry.publish(42)
	if parts, err := entry.waitRequest(); err != nil || len(parts) != 0 {
		t.Fatalf("waitRequest() = (%v, %v), want (empty, nil)", parts, err)
	}
	entry.waitSettled()
}

func TestCompletionEntryDropsLateRequestPartsAfterCallerCancellation(t *testing.T) {
	waitCtx, cancel := context.WithCancel(context.Background())
	entry := newCompletionEntry(completionRequest, waitCtx)
	cancel()
	if _, err := entry.waitRequest(); !errors.Is(err, context.Canceled) {
		t.Fatalf("waitRequest() error = %v, want context.Canceled", err)
	}
	part, err := NewMessage([]byte("late"))
	if err != nil {
		t.Fatalf("NewMessage() error = %v", err)
	}
	entry.capture([]*Message{part}, nil)
	entry.publish(43)
	entry.waitSettled()
	if part.Data() != nil {
		t.Fatal("late request payload was not closed")
	}
}

func TestRuntimeOwnerReusesPollerAcrossCompletedRequests(t *testing.T) {
	ctx, err := NewContext()
	if err != nil {
		t.Fatal(err)
	}
	defer ctx.Close()
	server, err := ctx.RouterSocket()
	if err != nil {
		t.Fatal(err)
	}
	defer server.Close()
	client, err := ctx.DealerSocket()
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	endpoint := "inproc://completion-owner-reuse"
	if err := server.Bind(endpoint); err != nil {
		t.Fatal(err)
	}
	if err := client.Connect(endpoint); err != nil {
		t.Fatal(err)
	}
	done := make(chan error, 1)
	go func() {
		for i := 0; i < 20; i++ {
			var r Received
			ok, err := server.Recv(&r, RecvFlagsNone)
			if err != nil || !ok {
				done <- err
				return
			}
			err = r.Reply().Message(r.Parts()[0]).Submit(context.Background())
			r.Close()
			if err != nil {
				done <- err
				return
			}
		}
		done <- nil
	}()
	var first *runtimeCompletionDrain
	for i := 0; i < 20; i++ {
		parts, err := client.Request().Bytes([]byte("echo")).Submit(context.Background())
		if err != nil {
			t.Fatal(err)
		}
		if len(parts) != 1 || string(parts[0].Data()) != "echo" {
			t.Fatal("reply payload changed")
		}
		MultipartClose(parts)
		owner := client.completion
		owner.mu.Lock()
		current := owner.runtime
		owner.mu.Unlock()
		if current == nil {
			t.Fatal("completed request discarded socket-owned runtime")
		}
		if first == nil {
			first = current
		} else if first != current {
			t.Fatal("request created a second runtime poller")
		}
	}
	if err := <-done; err != nil {
		t.Fatal(err)
	}
	if err := client.Close(); err != nil {
		t.Fatal(err)
	}
	select {
	case <-first.done:
	default:
		t.Fatal("socket close did not join its idle runtime")
	}
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
		if err = sender.Send().MoveMessage(body).Message(tail).Submit(context.Background()); err != nil {
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
	// admitting a send must not reintroduce completion entries/channels/handles.
	if allocations := testing.AllocsPerRun(100, exchange); allocations > 22 {
		t.Fatalf("immediate two-part send/receive allocated %.0f objects, budget 22", allocations)
	}
}
