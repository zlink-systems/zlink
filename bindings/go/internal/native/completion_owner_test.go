package native

import (
	"context"
	"errors"
	"syscall"
	"testing"
)

func TestImmediateSendRegistrationDoesNotStartRuntimeDrain(t *testing.T) {
	owner := newCompletionOwner(nil)
	entry := newSendCompletionEntry(context.Background(), nil)
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
	defer entry.deleteHandle()
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
	defer entry.deleteHandle()
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

func TestInactiveRuntimeKeepsOwnershipWhenSubmitRegistersBeforeRetire(t *testing.T) {
	owner := newCompletionOwner(nil)
	runtime := &runtimeCompletionDrain{}
	owner.runtime = runtime

	if _, active := owner.runtimeEvents(runtime); active {
		t.Fatal("empty completion owner unexpectedly reported an active runtime")
	}
	entry := &completionEntry{handleKey: 1}
	owner.entries[entry.handleKey] = entry

	if owner.exitInactiveRuntime(runtime) {
		t.Fatal("runtime exited after an entry registered in the idle-retirement window")
	}
	if owner.runtime != runtime {
		t.Fatal("runtime ownership was cleared while a registered entry still needed it")
	}
}
