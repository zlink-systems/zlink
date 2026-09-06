// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"
*/
import "C"

import (
	"strings"
	"sync"
	"sync/atomic"
	"time"
	"unsafe"
)

type socketCore struct {
	handle     atomic.Pointer[byte]
	closeMu    sync.Mutex
	context    *Context
	completion *completionOwner
}

func newSocketCore(ctx *Context, socketType C.zlink_socket_type_t) (*socketCore, error) {
	if ctx == nil || ctx.closed.Load() {
		return nil, &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	handle := C.zlink_socket(ctx.raw(), socketType)
	if handle == nil {
		return nil, configErrorFromErrno(currentErrno())
	}
	core := &socketCore{context: ctx}
	core.handle.Store((*byte)(handle))
	core.completion = newCompletionOwner(handle)
	ctx.registerSocket(core)
	return core, nil
}

func (s *socketCore) raw() unsafe.Pointer {
	if s == nil {
		return nil
	}
	return unsafe.Pointer(s.handle.Load())
}

func (s *socketCore) isClosed() bool {
	return s == nil || s.raw() == nil
}

func (s *socketCore) Bind(endpoint string) error {
	return s.withCString(endpoint, func(cstr *C.char) error {
		return bindErrorFromResult(C.zlink_bind(s.raw(), cstr))
	})
}

func (s *socketCore) Connect(endpoint string) error {
	return s.withCString(endpoint, func(cstr *C.char) error {
		return connectErrorFromResult(C.zlink_connect(s.raw(), cstr))
	})
}

func (s *socketCore) Unbind(endpoint string) error {
	return s.withCString(endpoint, func(cstr *C.char) error {
		return connectErrorFromResult(C.zlink_unbind(s.raw(), cstr))
	})
}

func (s *socketCore) Disconnect(endpoint string) error {
	return s.withCString(endpoint, func(cstr *C.char) error {
		return connectErrorFromResult(C.zlink_disconnect(s.raw(), cstr))
	})
}

func (s *socketCore) DisconnectRID(peerRID RoutingID) error {
	rid := peerRID.toC()
	return connectErrorFromResult(
		C.zlink_disconnect_rid(
			s.raw(),
			(*C.zlink_routing_id_t)(unsafe.Pointer(&rid)),
		))
}

func (s *socketCore) Close() error {
	if s == nil {
		return nil
	}
	s.closeMu.Lock()
	defer s.closeMu.Unlock()
	if s.raw() == nil {
		return nil
	}
	s.completion.shutdownOwner()
	handle := s.raw()
	closeErr := closeErrorFromResult(C.zlink_close(handle))
	if closeErr != nil {
		return closeErr
	}
	s.handle.Store(nil)
	if s.context != nil {
		s.context.unregisterSocket(s)
	}
	return nil
}

func (s *socketCore) completionDrainOwner() *completionOwner {
	if s == nil {
		return nil
	}
	return s.completion
}

func (s *socketCore) setIntOption(option C.zlink_option_t, value int32) error {
	return s.setOption(option, unsafe.Pointer(&value), C.size_t(C.sizeof_int))
}

func (s *socketCore) setUint64Option(option C.zlink_option_t, value uint64) error {
	raw := C.uint64_t(value)
	return s.setOption(option, unsafe.Pointer(&raw), C.size_t(unsafe.Sizeof(raw)))
}

func (s *socketCore) getIntOption(option C.zlink_option_t) (int32, error) {
	var value C.int
	size := C.size_t(C.sizeof_int)
	err := configErrorFromResult(C.zlink_get_option(s.raw(), option, unsafe.Pointer(&value), &size))
	return int32(value), err
}

func (s *socketCore) getUint64Option(option C.zlink_option_t) (uint64, error) {
	var value C.uint64_t
	size := C.size_t(unsafe.Sizeof(value))
	err := configErrorFromResult(C.zlink_get_option(s.raw(), option, unsafe.Pointer(&value), &size))
	return uint64(value), err
}

func (s *socketCore) setInt64Option(option C.zlink_option_t, value int64) error {
	raw := C.int64_t(value)
	return s.setOption(option, unsafe.Pointer(&raw), C.size_t(unsafe.Sizeof(raw)))
}

func (s *socketCore) getInt64Option(option C.zlink_option_t) (int64, error) {
	var value C.int64_t
	size := C.size_t(unsafe.Sizeof(value))
	err := configErrorFromResult(C.zlink_get_option(s.raw(), option, unsafe.Pointer(&value), &size))
	return int64(value), err
}

func (s *socketCore) setBoolOption(option C.zlink_option_t, value bool) error {
	var raw C.int
	if value {
		raw = 1
	}
	return s.setOption(option, unsafe.Pointer(&raw), C.size_t(C.sizeof_int))
}

func (s *socketCore) getBoolOption(option C.zlink_option_t) (bool, error) {
	value, err := s.getIntOption(option)
	return value != 0, err
}

func (s *socketCore) setStringOption(option C.zlink_option_t, value string) error {
	if strings.IndexByte(value, 0) >= 0 {
		return validationError("string contains null byte")
	}
	cstr := C.CString(value)
	defer C.free(unsafe.Pointer(cstr))
	return s.setOption(option, unsafe.Pointer(cstr), C.size_t(len(value)))
}

func (s *socketCore) getStringOption(option C.zlink_option_t, capHint int) (string, error) {
	if capHint <= 0 {
		capHint = 256
	}
	buf := make([]byte, capHint)
	size := C.size_t(len(buf))
	err := configErrorFromResult(C.zlink_get_option(s.raw(), option, unsafe.Pointer(&buf[0]), &size))
	if err != nil {
		return "", err
	}
	return string(buf[:int(size)]), nil
}

func (s *socketCore) setOption(option C.zlink_option_t, ptr unsafe.Pointer, size C.size_t) error {
	if s == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	return setNativeOption(s.raw(), s.isClosed(), option, ptr, size)
}

func (s *socketCore) setDurationOption(option C.zlink_option_t, value time.Duration) error {
	return setNativeDurationOption(s.raw(), s.isClosed(), option, value)
}

func (s *socketCore) getDurationOption(option C.zlink_option_t) (time.Duration, error) {
	value, err := s.getIntOption(option)
	return time.Duration(value) * time.Millisecond, err
}

func (s *socketCore) withCString(value string, fn func(*C.char) error) error {
	if s.isClosed() {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	if err := validateEndpointString(value); err != nil {
		return err
	}
	cstr := C.CString(value)
	defer C.free(unsafe.Pointer(cstr))
	return fn(cstr)
}
