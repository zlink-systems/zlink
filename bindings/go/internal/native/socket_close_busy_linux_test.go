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

func TestBusySocketCloseKeepsCompletionEntryAlive(t *testing.T) {
	ctx, err := NewContext()
	if err != nil {
		t.Fatal(err)
	}
	defer ctx.Close()
	socket, err := ctx.DealerSocket()
	if err != nil {
		t.Fatal(err)
	}
	core := socket.socketCore
	poller, err := NewPoller()
	if err != nil {
		t.Fatal(err)
	}
	defer poller.Close()
	if err := poller.AddSocket(socket, PollCompletion, 1); err != nil {
		t.Fatal(err)
	}
	completiontest.Start(core.raw())
	defer completiontest.Stop()
	submission, err := socket.Request().Bytes([]byte("pending")).Submit(context.Background())
	if err != nil || submission.Result() != SubmitOK {
		t.Fatalf("Request Submit() = (%v, %v)", submission, err)
	}
	entry := submission.(*requestSubmission).entry
	completiontest.OverrideCloseBusyOnce()
	var closeErr *CloseError
	if err := socket.Close(); !errors.As(err, &closeErr) || closeErr.Result != CloseBusy || !errors.Is(err, syscall.EBUSY) {
		t.Fatalf("Close() = %v, want Core CloseBusy/EBUSY", err)
	}
	core.completion.mu.Lock()
	kept := !core.completion.shutdown && core.completion.entries[entry.handleKey] == entry
	core.completion.mu.Unlock()
	if !kept || core.isClosed() {
		t.Fatal("busy close removed the live socket or its completion entry")
	}
	select {
	case <-entry.done:
		t.Fatal("busy close terminated the in-flight entry")
	default:
	}
	if drained, err := core.completion.drain(true); err != nil || drained.requestCompletions != 1 {
		t.Fatalf("drain after busy close = (%+v, %v)", drained, err)
	}
	parts, err := submission.Reply(context.Background())
	if err != nil || len(parts) != 0 {
		t.Fatalf("reply after busy close = (%v, %v)", parts, err)
	}
	if err := socket.Close(); err != nil {
		t.Fatalf("Close() retry = %v", err)
	}
}
