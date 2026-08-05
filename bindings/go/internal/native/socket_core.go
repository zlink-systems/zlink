// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"
*/
import "C"

import (
	"math"
	"runtime/cgo"
	"strings"
	"sync"
	"sync/atomic"
	"time"
	"unsafe"
)

type socketCore struct {
	handle                  atomic.Pointer[byte]
	closed                  atomic.Bool
	callbackMu              sync.Mutex
	recvHandle              cgo.Handle
	recvActive              atomic.Uintptr
	completionControlHandle cgo.Handle
	subscribeHandle         cgo.Handle
	sendReadyHandle         cgo.Handle
	streamPacketHandle      cgo.Handle
	requestProgressMu       sync.Mutex
	requestProgress         *progressPump
	requestDispatcher       *callbackDispatcher
}

func newSocketCore(ctx *Context, socketType C.zlink_socket_type_t) (*socketCore, error) {
	if ctx == nil || ctx.closed.Load() {
		return nil, &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	handle := C.zlink_socket(ctx.raw(), socketType)
	if handle == nil {
		return nil, configErrorFromErrno(currentErrno())
	}
	core := &socketCore{}
	core.handle.Store((*byte)(handle))
	return core, nil
}

func (s *socketCore) raw() unsafe.Pointer {
	if s == nil {
		return nil
	}
	return unsafe.Pointer(s.handle.Load())
}

func (s *socketCore) isClosed() bool {
	return s == nil || s.closed.Load() || s.raw() == nil
}

func (s *socketCore) startRequestProgress(state *replyCallbackState) {
	if s == nil || state == nil {
		return
	}
	s.callbackMu.Lock()
	defer s.callbackMu.Unlock()
	handle := s.raw()
	if s.isClosed() || handle == nil || externalRequestProgressActive(handle) {
		return
	}
	s.requestProgressMu.Lock()
	if s.requestProgress == nil {
		s.requestProgress = newProgressPump(handle)
	}
	pump := s.requestProgress
	pump.attach(state)
	s.requestProgressMu.Unlock()
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
	s.callbackMu.Lock()
	if s.closed.Load() {
		s.callbackMu.Unlock()
		return nil
	}
	s.requestProgressMu.Lock()
	pump := s.requestProgress
	s.requestProgressMu.Unlock()
	if pump != nil {
		pump.stopAndWait()
	}
	handle := s.raw()
	if err := closeErrorFromResult(C.zlink_close(handle)); err != nil {
		if pump != nil && !externalRequestProgressActive(handle) {
			pump.resume()
		}
		s.callbackMu.Unlock()
		return err
	}
	s.closed.Store(true)
	s.handle.Store(nil)
	s.requestProgressMu.Lock()
	if s.requestProgress == pump {
		s.requestProgress = nil
	}
	s.requestProgressMu.Unlock()
	requestDispatcher := s.requestDispatcher
	s.requestDispatcher = nil
	s.callbackMu.Unlock()
	if requestDispatcher != nil {
		requestDispatcher.close()
	}
	s.releaseCallbacks()
	return nil
}

func (s *socketCore) releaseCallbacks() {
	s.callbackMu.Lock()
	handles := []cgo.Handle{s.recvHandle, s.completionControlHandle, s.subscribeHandle, s.sendReadyHandle, s.streamPacketHandle}
	s.recvHandle = 0
	s.recvActive.Store(0)
	s.completionControlHandle = 0
	s.subscribeHandle = 0
	s.sendReadyHandle = 0
	s.streamPacketHandle = 0
	s.callbackMu.Unlock()
	for _, handle := range handles {
		releaseCallbackHandle(handle)
	}
}

func (s *socketCore) pauseInternalRequestProgress() {
	if s == nil {
		return
	}
	s.callbackMu.Lock()
	defer s.callbackMu.Unlock()
	s.requestProgressMu.Lock()
	pump := s.requestProgress
	s.requestProgressMu.Unlock()
	if pump != nil {
		pump.stopAndWait()
	}
}

func (s *socketCore) resumeInternalRequestProgress() {
	if s == nil {
		return
	}
	s.callbackMu.Lock()
	defer s.callbackMu.Unlock()
	handle := s.raw()
	if s.isClosed() || handle == nil || externalRequestProgressActive(handle) {
		return
	}
	s.requestProgressMu.Lock()
	pump := s.requestProgress
	s.requestProgressMu.Unlock()
	if pump != nil {
		pump.resume()
	}
}

func (s *socketCore) hasReceiveHandler() bool {
	return s.recvActive.Load() != 0
}

func (s *socketCore) requestCallbackDispatcher() *callbackDispatcher {
	if s == nil {
		return nil
	}
	s.callbackMu.Lock()
	defer s.callbackMu.Unlock()
	if s.isClosed() {
		return nil
	}
	if s.requestDispatcher == nil {
		s.requestDispatcher = newCallbackDispatcher()
	}
	return s.requestDispatcher
}

func (s *socketCore) replaceCallback(handle cgo.Handle, slot *cgo.Handle, active *atomic.Uintptr, register func() error) error {
	s.callbackMu.Lock()
	if s.isClosed() {
		s.callbackMu.Unlock()
		return &HandlerError{Result: HandlerInvalidArgument, nativeErrno: int(C.EFAULT)}
	}
	if err := register(); err != nil {
		s.callbackMu.Unlock()
		return err
	}
	previous := *slot
	*slot = handle
	if active != nil {
		active.Store(uintptr(handle))
	}
	s.callbackMu.Unlock()
	if previous != 0 {
		releaseCallbackHandle(previous)
	}
	return nil
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

func (s *socketCore) setHwmOption(option C.zlink_option_t, value int) error {
	if value < 0 {
		return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	return s.setUint64Option(option, uint64(value))
}

func (s *socketCore) getHwmOption(option C.zlink_option_t) (int, error) {
	value, err := s.getUint64Option(option)
	if err != nil {
		return 0, err
	}
	if value > uint64(math.MaxInt) {
		return 0, &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EOVERFLOW)}
	}
	return int(value), nil
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
