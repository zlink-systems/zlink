package main

import (
	"errors"
	"syscall"
	"testing"
	"time"

	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/perf/internal/perfcommon"
)

type phaseMarkerSubmitError struct {
	syscall.Errno
	result zlink.SubmitResult
}

func (e phaseMarkerSubmitError) Code() int          { return int(e.result) }
func (e phaseMarkerSubmitError) InternalErrno() int { return int(e.Errno) }

func TestPubSubPhaseMarkerPublish(t *testing.T) {
	transient := phaseMarkerSubmitError{syscall.EINTR, zlink.SubmitInternalError}
	timedOut := phaseMarkerSubmitError{syscall.ETIMEDOUT, zlink.SubmitInternalError}
	fatal := phaseMarkerSubmitError{syscall.ENOTCONN, zlink.SubmitNotConnected}
	for _, tc := range []struct {
		name          string
		firstErr      error
		wantErr       error
		attempts      int
		neverAdmitted bool
	}{
		{name: "interrupted", firstErr: transient, attempts: 2},
		{name: "timed out", firstErr: timedOut, attempts: 2},
		{name: "backpressured", attempts: 2},
		{name: "disconnected", firstErr: fatal, wantErr: fatal, attempts: 1},
		{name: "exhausted", wantErr: syscall.ETIMEDOUT, attempts: 5, neverAdmitted: true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			attempts := 0
			sendTimeout := 200 * time.Millisecond
			setSendTimeout := func(timeout time.Duration) error {
				sendTimeout = timeout
				return nil
			}
			err := publishMultiPubSubPhaseMarker(64, perfcommon.PhaseActive, setSendTimeout, func(marker *zlink.Message) (bool, error) {
				attempts++
				if sendTimeout != 2*time.Second {
					t.Errorf("attempt %d send timeout = %s, want 2s", attempts, sendTimeout)
				}
				header, ok := perfcommon.DecodeMetricHeader(marker.Data())
				if !ok || header.Phase != perfcommon.PhaseActive || header.MsgSize != 64 || header.Sequence != 0 || header.SentTsNs != 0 {
					t.Fatalf("invalid phase marker on attempt %d: %+v", attempts, header)
				}
				if attempts == 1 || tc.neverAdmitted {
					return false, tc.firstErr
				}
				_ = marker.Close()
				return true, nil
			})
			if !errors.Is(err, tc.wantErr) || attempts != tc.attempts {
				t.Fatalf("attempts = %d, error = %v; want %d attempts and %v", attempts, err, tc.attempts, tc.wantErr)
			}
		})
	}
}
