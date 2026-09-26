//go:build linux

// SPDX-License-Identifier: MPL-2.0

package native

import (
	"errors"
	"os"
	"syscall"
	"testing"
	"time"

	"zlink.systems/zlink/internal/native/completiontest"
)

func TestPollerCloseDuringWaitReportsBusyAndCanRetry(t *testing.T) {
	poller, err := NewPoller()
	if err != nil {
		t.Fatal(err)
	}
	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer reader.Close()
	defer writer.Close()
	probe, probeWriter, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer probe.Close()
	defer probeWriter.Close()
	if err := poller.AddFd(int(reader.Fd()), PollIn, 1); err != nil {
		t.Fatal(err)
	}
	woken := make(chan pollerWaitResult, 1)
	go func() {
		n, err := poller.Wait(make([]PollEvent, 1), 10*time.Second)
		woken <- pollerWaitResult{n: n, err: err}
	}()
	for {
		err := poller.AddFd(int(probe.Fd()), PollIn, 2)
		var configErr *ConfigError
		if errors.As(err, &configErr) && configErr.Result == ConfigBusy {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		if err := poller.RemoveFd(int(probe.Fd())); err != nil {
			if errors.As(err, &configErr) && configErr.Result == ConfigBusy {
				break
			}
			t.Fatal(err)
		}
	}
	var waitErr *ConfigError
	if _, err := poller.Wait(make([]PollEvent, 1), 0); !errors.As(err, &waitErr) || waitErr.Result != ConfigBusy || !errors.Is(err, syscall.EBUSY) {
		t.Fatalf("overlapping Wait = %v, want Core ConfigBusy/EBUSY", err)
	}
	var closeErr *CloseError
	if err := poller.Close(); !errors.As(err, &closeErr) || closeErr.Result != CloseBusy || !errors.Is(err, syscall.EBUSY) {
		t.Fatalf("Close during Wait = %v, want CloseBusy/EBUSY", err)
	}
	var sizeErr *ConfigError
	if _, err := poller.Size(); !errors.As(err, &sizeErr) || sizeErr.Result != ConfigBusy {
		t.Fatalf("Size after Busy close = %v, want ConfigBusy from the live poller", err)
	}
	if _, err := writer.Write([]byte{1}); err != nil {
		t.Fatal(err)
	}
	if got := <-woken; got.err != nil || got.n != 1 {
		t.Fatalf("Wait = (%d, %v), want one event", got.n, got.err)
	}
	if err := poller.Close(); err != nil {
		t.Fatalf("Close after Wait = %v", err)
	}
}

func TestPollerWaitEmptyBufferPreservesNativeResult(t *testing.T) {
	poller, err := NewPoller()
	if err != nil {
		t.Fatal(err)
	}
	defer poller.Close()
	completiontest.OverrideEmptyPollerWaitOnce()
	var configErr *ConfigError
	if _, err := poller.Wait(nil, 0); !errors.As(err, &configErr) || configErr.Result != ConfigBusy || !errors.Is(err, syscall.EBUSY) {
		t.Fatalf("empty Wait = %v, want native ConfigBusy/EBUSY", err)
	}
	if _, err := poller.Wait(nil, 0); !errors.As(err, &configErr) || configErr.Result != ConfigInvalidArgument || !errors.Is(err, syscall.EINVAL) {
		t.Fatalf("empty Wait = %v, want Core ConfigInvalidArgument/EINVAL", err)
	}
}

func TestPollerModifyMissingSourceUsesNativeResult(t *testing.T) {
	ctx, err := NewContext()
	if err != nil {
		t.Fatal(err)
	}
	defer ctx.Close()
	socket, err := ctx.PairSocket()
	if err != nil {
		t.Fatal(err)
	}
	defer socket.Close()
	poller, err := NewPoller()
	if err != nil {
		t.Fatal(err)
	}
	defer poller.Close()
	completiontest.OverrideMissingPollerModifyOnce()
	var configErr *ConfigError
	if err := poller.ModifySocket(socket, PollIn); !errors.As(err, &configErr) || configErr.Result != ConfigBusy || !errors.Is(err, syscall.EBUSY) {
		t.Fatalf("ModifySocket missing source = %v, want native ConfigBusy/EBUSY", err)
	}
	if err := poller.ModifySocket(socket, PollIn); !errors.As(err, &configErr) || configErr.Result != ConfigNotFound || !errors.Is(err, syscall.ENOENT) {
		t.Fatalf("ModifySocket missing source = %v, want Core ConfigNotFound/ENOENT", err)
	}
}

func TestPollPreservesNativeErrorOutWithoutErrnoClassification(t *testing.T) {
	completiontest.OverridePollFailureOnce()
	var configErr *ConfigError
	if _, err := Poll(nil, 0); !errors.As(err, &configErr) || configErr.Result != ConfigOK || !errors.Is(err, syscall.EBUSY) {
		t.Fatalf("Poll failure = %v, want unmodified native ConfigOK/EBUSY", err)
	}
}
