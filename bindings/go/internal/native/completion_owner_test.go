package native

import (
	"context"
	"errors"
	"testing"
)

func TestCompletionEntryJoinsCaptureBeforePublish(t *testing.T) {
	entry := newCompletionEntry(completionSend, context.Background())
	defer entry.deleteHandle()
	entry.capture(nil, nil)
	select {
	case <-entry.done:
		t.Fatal("capture must not settle before submit publishes its completion id")
	default:
	}
	entry.publish(42)
	if err := entry.waitSend(); err != nil {
		t.Fatalf("waitSend() error = %v", err)
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
