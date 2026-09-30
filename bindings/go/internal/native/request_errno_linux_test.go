//go:build linux

// SPDX-License-Identifier: MPL-2.0

package native

import (
	"syscall"
	"testing"
)

// Core 03-errors §3의 REQUEST result별 대표 errno와 같아야 한다.
func TestFallbackRequestErrnoMatchesCoreRepresentative(t *testing.T) {
	const efsm = 156384763
	const eterm = 156384765
	cases := []struct {
		result RequestResult
		errno  int
	}{
		{RequestTimedOut, int(syscall.ETIMEDOUT)},
		{RequestNotFound, int(syscall.ENOENT)},
		{RequestTerminated, eterm},
		{RequestRejected, int(syscall.EACCES)},
		{RequestConflict, int(syscall.EEXIST)},
		{RequestBusy, int(syscall.EBUSY)},
		{RequestNotConnected, int(syscall.ENOTCONN)},
		{RequestInvalidArgument, int(syscall.EINVAL)},
		{RequestInvalidState, efsm},
		{RequestNotSupported, int(syscall.ENOTSUP)},
		{RequestBackpressured, int(syscall.EAGAIN)},
	}
	for _, c := range cases {
		if got := fallbackRequestErrno(c.result); got != c.errno {
			t.Errorf("fallbackRequestErrno(%v) = %d, want %d", c.result, got, c.errno)
		}
	}
}
