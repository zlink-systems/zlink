// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"

extern void zlinkGoThreadInvoke(void *);
static inline void *zlink_thread_start_go(uintptr_t arg) {
    return zlink_thread_start(zlinkGoThreadInvoke, (void *)arg);
}
*/
import "C"

import (
	"runtime/cgo"
	"strings"
	"sync"
	"unsafe"
)

type SocketTarget interface {
	raw() unsafe.Pointer
}

func Has(capability string) (bool, error) {
	if strings.IndexByte(capability, 0) >= 0 {
		return false, &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	cstr := C.CString(capability)
	defer C.free(unsafe.Pointer(cstr))
	return bool(C.zlink_has(cstr)), nil
}

func Proxy(frontend SocketTarget, backend SocketTarget, capture SocketTarget) error {
	frontendHandle, err := socketHandle(frontend)
	if err != nil {
		return err
	}
	backendHandle, err := socketHandle(backend)
	if err != nil {
		return err
	}
	var captureHandle unsafe.Pointer
	if capture != nil {
		captureHandle, err = socketHandle(capture)
		if err != nil {
			return err
		}
	}
	return configErrorFromResult(ConfigResult(C.zlink_proxy(frontendHandle, backendHandle, captureHandle)))
}

func ProxySteerable(frontend SocketTarget, backend SocketTarget, capture SocketTarget, control SocketTarget) error {
	frontendHandle, err := socketHandle(frontend)
	if err != nil {
		return err
	}
	backendHandle, err := socketHandle(backend)
	if err != nil {
		return err
	}
	var captureHandle unsafe.Pointer
	if capture != nil {
		captureHandle, err = socketHandle(capture)
		if err != nil {
			return err
		}
	}
	controlHandle, err := socketHandle(control)
	if err != nil {
		return err
	}
	return configErrorFromResult(ConfigResult(C.zlink_proxy_steerable(frontendHandle, backendHandle, captureHandle, controlHandle)))
}

func socketHandle(socket SocketTarget) (unsafe.Pointer, error) {
	if socket == nil {
		return nil, &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	return socket.raw(), nil
}

func Sleep(seconds int) {
	C.zlink_sleep(C.int(seconds))
}

func MultipartClose(parts []*Message) {
	for _, part := range parts {
		if part != nil {
			_ = part.Close()
		}
	}
}

type Stopwatch struct {
	handle unsafe.Pointer
}

func NewStopwatch() *Stopwatch {
	handle := C.zlink_stopwatch_start()
	if handle == nil {
		return nil
	}
	return &Stopwatch{handle: handle}
}

func (s *Stopwatch) Intermediate() uint64 {
	if s == nil || s.handle == nil {
		return 0
	}
	return uint64(C.zlink_stopwatch_intermediate(s.handle))
}

func (s *Stopwatch) Stop() uint64 {
	if s == nil || s.handle == nil {
		return 0
	}
	handle := s.handle
	s.handle = nil
	return uint64(C.zlink_stopwatch_stop(handle))
}

type AtomicCounter struct {
	handle unsafe.Pointer
}

func NewAtomicCounter() *AtomicCounter {
	handle := C.zlink_atomic_counter_new()
	if handle == nil {
		return nil
	}
	return &AtomicCounter{handle: handle}
}

func (c *AtomicCounter) Set(value int) {
	if c == nil || c.handle == nil {
		return
	}
	C.zlink_atomic_counter_set(c.handle, C.int(value))
}

func (c *AtomicCounter) Increment() int {
	if c == nil || c.handle == nil {
		return 0
	}
	return int(C.zlink_atomic_counter_inc(c.handle))
}

func (c *AtomicCounter) Decrement() int {
	if c == nil || c.handle == nil {
		return 0
	}
	return int(C.zlink_atomic_counter_dec(c.handle))
}

func (c *AtomicCounter) Value() int {
	if c == nil || c.handle == nil {
		return 0
	}
	return int(C.zlink_atomic_counter_value(c.handle))
}

func (c *AtomicCounter) Close() {
	if c == nil || c.handle == nil {
		return
	}
	handle := c.handle
	c.handle = nil
	C.zlink_atomic_counter_destroy(&handle)
}

type Thread struct {
	handle unsafe.Pointer
	state  cgo.Handle
	once   sync.Once
	err    error
}

type threadState struct {
	target func()
	mu     sync.Mutex
	err    error
}

func (s *threadState) setErr(err error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.err = err
}

func (s *threadState) getErr() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.err
}

func NewThread(target func()) (*Thread, error) {
	if target == nil {
		return nil, &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	state := cgo.NewHandle(&threadState{target: target})
	handle := C.zlink_thread_start_go(C.uintptr_t(state))
	if handle == nil {
		state.Delete()
		return nil, &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.zlink_errno())}
	}
	return &Thread{handle: handle, state: state}, nil
}

func (t *Thread) Join() error {
	if t == nil {
		return nil
	}
	t.once.Do(func() {
		handle := t.handle
		if handle == nil {
			return
		}
		t.handle = nil
		C.zlink_thread_join(handle)
		if t.state != 0 {
			state := t.state.Value().(*threadState)
			t.err = state.getErr()
			t.state.Delete()
			t.state = 0
		}
	})
	return t.err
}

func (t *Thread) Close() error {
	return t.Join()
}
