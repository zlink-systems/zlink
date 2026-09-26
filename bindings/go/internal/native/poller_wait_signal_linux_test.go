//go:build linux

// SPDX-License-Identifier: MPL-2.0

package native

import (
	"os"
	"runtime"
	"syscall"
	"testing"
	"time"
)

type pollerWaitResult struct {
	n   int
	err error
}

// 05-polling §2, §9: a signal that interrupts zlink_poller_wait does not end
// it with EINTR. Without readiness the wait returns 0 after its timeout.
func TestPollerWaitInterruptedBySignalReturnsAtTimeout(t *testing.T) {
	ctx, err := NewContext()
	if err != nil {
		t.Fatal(err)
	}
	defer ctx.Close()
	poller, err := NewPoller()
	if err != nil {
		t.Fatal(err)
	}
	defer poller.Close()
	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer reader.Close()
	defer writer.Close()
	if err := poller.AddFd(int(reader.Fd()), PollIn, 1); err != nil {
		t.Fatal(err)
	}

	const timeout = 300 * time.Millisecond
	waiter := make(chan int, 1)
	result := make(chan pollerWaitResult, 1)
	go func() {
		// The wait runs on this OS thread, which receives the signals below.
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		waiter <- syscall.Gettid()
		n, err := poller.Wait(make([]PollEvent, 1), timeout)
		result <- pollerWaitResult{n: n, err: err}
	}()
	tid := <-waiter
	pid := os.Getpid()
	var got pollerWaitResult
	for done := false; !done; {
		select {
		case got = <-result:
			done = true
		default:
			// SIGURG is the Go runtime's preemption signal; its handler
			// returns, and it interrupts the native poll(2).
			if err := syscall.Tgkill(pid, tid, syscall.SIGURG); err != nil {
				t.Fatal(err)
			}
			runtime.Gosched()
		}
	}
	if got.err != nil || got.n != 0 {
		t.Fatalf("interrupted Wait() = (%d, %v), want (0, nil) at the timeout", got.n, got.err)
	}
}
