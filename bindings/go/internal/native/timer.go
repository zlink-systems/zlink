// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include "zlink.h"
*/
import "C"

import "unsafe"

type Timer struct {
	handle unsafe.Pointer
	closed bool
}

func NewTimer() (*Timer, error) {
	handle, cerr := C.zlink_timer_new()
	if handle == nil {
		return nil, configErrorFromErrno(cgoErrno(cerr))
	}
	return &Timer{handle: handle}, nil
}

func (t *Timer) raw() unsafe.Pointer {
	if t == nil {
		return nil
	}
	return t.handle
}

func (t *Timer) Start(intervalNs, repeatCount uint64) error {
	if t == nil || t.closed || t.handle == nil {
		return newConfigError(ConfigInvalidHandle, int(C.EFAULT))
	}
	nativeResult0, nativeErr0 := C.zlink_timer_start(t.handle, C.uint64_t(intervalNs), C.uint64_t(repeatCount))
	return configErrorFromCall(nativeResult0, nativeErr0)
}

func (t *Timer) Stop() error {
	if t == nil || t.closed || t.handle == nil {
		return newConfigError(ConfigInvalidHandle, int(C.EFAULT))
	}
	nativeResult1, nativeErr1 := C.zlink_timer_stop(t.handle)
	return configErrorFromCall(nativeResult1, nativeErr1)
}

// Recv drains the next timer fire. Returns (count, true, nil) when data is
// available, (0, false, nil) when no timer fire is pending (EAGAIN), or
// (0, false, err) on error. Value-return form is allowed for monitor/timer
// control-plane APIs by doc/spec/bindings/go/README.md §Receive And Subscribe Shape.
func (t *Timer) Recv() (uint64, bool, error) {
	if t == nil || t.closed || t.handle == nil {
		return 0, false, newRecvError(RecvTerminated, int(C.EFAULT))
	}
	var fireCount C.uint64_t
	rc, cerr := C.zlink_timer_recv(t.handle, &fireCount)
	if rc == C.zlink_recv_result_t(RecvNoData) {
		return 0, false, nil
	}
	if err := recvErrorFromCall(rc, cerr); err != nil {
		return 0, false, err
	}
	return uint64(fireCount), true, nil
}

func (t *Timer) Close() error {
	if t == nil || t.closed || t.handle == nil {
		return nil
	}
	handle := t.handle
	nativeResult2, nativeErr2 := C.zlink_timer_destroy(&handle)
	if err := closeErrorFromCall(nativeResult2, nativeErr2); err != nil {
		return err
	}
	t.handle = nil
	t.closed = true
	return nil
}
