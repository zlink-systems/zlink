//go:build linux

// SPDX-License-Identifier: MPL-2.0

package native

import (
	"os"
	"testing"
)

func TestPollerWaitReusesEventBuffers(t *testing.T) {
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
	if _, err := writer.Write([]byte{1}); err != nil {
		t.Fatal(err)
	}
	events := make([]PollEvent, 1)
	allocations := testing.AllocsPerRun(100, func() {
		if count, err := poller.Wait(events, 0); err != nil || count != 1 {
			t.Fatalf("Wait = (%d, %v), want one event", count, err)
		}
	})
	t.Logf("steady-state Poller.Wait allocations: %.2f per call", allocations)
	if allocations != 0 {
		t.Fatalf("steady-state Poller.Wait allocations = %.2f, want zero", allocations)
	}
}
