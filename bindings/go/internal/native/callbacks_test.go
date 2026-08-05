// SPDX-License-Identifier: MPL-2.0

package native

import (
	"runtime"
	"testing"
	"time"
)

func TestCallbackDispatcherCloseIsReentrantAfterExternalClose(t *testing.T) {
	dispatcher := newCallbackDispatcher()
	started := make(chan struct{})
	release := make(chan struct{})
	finished := make(chan struct{})
	if !dispatcher.enqueue(&callbackTask{invoke: func() {
		close(started)
		<-release
		dispatcher.close()
		close(finished)
	}}) {
		t.Fatal("callback dispatcher rejected the task")
	}
	<-started

	externalDone := make(chan struct{})
	go func() {
		dispatcher.close()
		close(externalDone)
	}()
	deadline := time.Now().Add(time.Second)
	for {
		dispatcher.mu.Lock()
		closed := dispatcher.closed
		dispatcher.mu.Unlock()
		if closed {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("external close did not close the dispatcher")
		}
		runtime.Gosched()
	}
	close(release)

	select {
	case <-finished:
	case <-time.After(time.Second):
		t.Fatal("reentrant callback dispatcher close deadlocked")
	}
	select {
	case <-externalDone:
	case <-time.After(time.Second):
		t.Fatal("external callback dispatcher close did not finish")
	}
}
