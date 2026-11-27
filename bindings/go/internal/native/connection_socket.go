// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"
*/
import "C"

import (
	"time"
	"unsafe"
)

type connectionSocket struct {
	*socketCore
}

// SetSendHighWaterMark sets the send byte HWM over the full uint64 range.
//
// Deprecated: use CommonOptions().SetSendHighWaterMark.
func (s *connectionSocket) SetSendHighWaterMark(value uint64) error {
	return s.CommonOptions().SetSendHighWaterMark(value)
}

// SendHighWaterMark returns the send byte HWM without narrowing the Core value.
//
// Deprecated: use CommonOptions().SendHighWaterMark.
func (s *connectionSocket) SendHighWaterMark() (uint64, error) {
	return s.CommonOptions().SendHighWaterMark()
}

// SetReceiveHighWaterMark sets the receive byte HWM over the full uint64 range.
//
// Deprecated: use CommonOptions().SetReceiveHighWaterMark.
func (s *connectionSocket) SetReceiveHighWaterMark(value uint64) error {
	return s.CommonOptions().SetReceiveHighWaterMark(value)
}

// ReceiveHighWaterMark returns the receive byte HWM without narrowing the Core value.
//
// Deprecated: use CommonOptions().ReceiveHighWaterMark.
func (s *connectionSocket) ReceiveHighWaterMark() (uint64, error) {
	return s.CommonOptions().ReceiveHighWaterMark()
}

// Deprecated: use CommonOptions().SetLinger.
func (s *connectionSocket) SetLinger(value time.Duration) error {
	return s.CommonOptions().SetLinger(value)
}

// Deprecated: use CommonOptions().Linger.
func (s *connectionSocket) Linger() (time.Duration, error) { return s.CommonOptions().Linger() }

// Deprecated: use CommonOptions().SetReceiveTimeout.
func (s *connectionSocket) SetReceiveTimeout(value time.Duration) error {
	return s.CommonOptions().SetReceiveTimeout(value)
}

// Deprecated: use CommonOptions().ReceiveTimeout.
func (s *connectionSocket) ReceiveTimeout() (time.Duration, error) {
	return s.CommonOptions().ReceiveTimeout()
}

// Deprecated: use CommonOptions().SetSendTimeout.
func (s *connectionSocket) SetSendTimeout(value time.Duration) error {
	return s.CommonOptions().SetSendTimeout(value)
}

// Deprecated: use CommonOptions().SendTimeout.
func (s *connectionSocket) SendTimeout() (time.Duration, error) {
	return s.CommonOptions().SendTimeout()
}

// Deprecated: use CommonOptions().SetTCPKeepalive.
func (s *connectionSocket) SetTCPKeepalive(value bool) error {
	return s.CommonOptions().SetTCPKeepalive(value)
}

// Deprecated: use CommonOptions().TCPKeepalive.
func (s *connectionSocket) TCPKeepalive() (bool, error) { return s.CommonOptions().TCPKeepalive() }

// Deprecated: use CommonOptions().SetTCPNoDelay.
func (s *connectionSocket) SetTCPNoDelay(value bool) error {
	return s.CommonOptions().SetTCPNoDelay(value)
}

// Deprecated: use CommonOptions().TCPNoDelay.
func (s *connectionSocket) TCPNoDelay() (bool, error) { return s.CommonOptions().TCPNoDelay() }

// Deprecated: use CommonOptions().SetIPv6.
func (s *connectionSocket) SetIPv6(value bool) error {
	return s.CommonOptions().SetIPv6(value)
}

// Deprecated: use CommonOptions().IPv6.
func (s *connectionSocket) IPv6() (bool, error) { return s.CommonOptions().IPv6() }

// Deprecated: use CommonOptions().SetRidDuplicatePolicy.
func (s *connectionSocket) SetRidDuplicatePolicy(value int) error {
	return s.CommonOptions().SetRidDuplicatePolicy(RidDuplicatePolicy(value))
}

// Deprecated: use CommonOptions().RidDuplicatePolicy.
func (s *connectionSocket) RidDuplicatePolicy() (int, error) {
	value, err := s.CommonOptions().RidDuplicatePolicy()
	return int(value), err
}

// Deprecated: use CommonOptions().SetSubmitRetryMode.
func (s *connectionSocket) SetSubmitRetryMode(value SubmitRetryMode) error {
	return s.CommonOptions().SetSubmitRetryMode(value)
}

// Deprecated: use CommonOptions().SubmitRetryMode.
func (s *connectionSocket) SubmitRetryMode() (SubmitRetryMode, error) {
	return s.CommonOptions().SubmitRetryMode()
}

// Deprecated: use CommonOptions().SetSubmitRetryTimeout.
func (s *connectionSocket) SetSubmitRetryTimeout(value time.Duration) error {
	return s.CommonOptions().SetSubmitRetryTimeout(value)
}

// Deprecated: use CommonOptions().SubmitRetryTimeout.
func (s *connectionSocket) SubmitRetryTimeout() (time.Duration, error) {
	return s.CommonOptions().SubmitRetryTimeout()
}

// Deprecated: use CommonOptions().SetSubmitRetryAttempts.
func (s *connectionSocket) SetSubmitRetryAttempts(value int) error {
	return s.CommonOptions().SetSubmitRetryAttempts(value)
}

// Deprecated: use CommonOptions().SubmitRetryAttempts.
func (s *connectionSocket) SubmitRetryAttempts() (int, error) {
	return s.CommonOptions().SubmitRetryAttempts()
}

// SetReceiveFlowState sets this socket's local receive-flow state
// (core-byte-hwm-flow-control-plan.ko.md §5). RUNNING/PAUSED is an absolute
// state, not a counter: repeating the current state succeeds as a no-op.
// Only DEALER and ROUTER sockets support this; every other socket type
// reports ConfigNotSupported and keeps its existing byte HWM and transport
// backpressure unchanged.
func (s *connectionSocket) SetReceiveFlowState(value ReceiveFlowState) error {
	return setNativeReceiveFlowState(s.raw(), s.socketCore.isClosed(), value)
}

func (s *connectionSocket) CommonOptions() *CommonSocketOptions {
	return &CommonSocketOptions{socket: s}
}

func (s *connectionSocket) LastEndpoint() (string, error) {
	return s.getStringOption(C.ZLINK_OPT_LAST_ENDPOINT, 1024)
}

func (s *connectionSocket) setPubBoolOption(option C.zlink_pub_option_t, value bool) error {
	return setNativePubBoolOption(s.raw(), s.socketCore.isClosed(), option, value)
}

func (s *connectionSocket) getPubBoolOption(option C.zlink_pub_option_t) (bool, error) {
	return getNativePubBoolOption(s.raw(), s.socketCore.isClosed(), option)
}

func (s *connectionSocket) getPubIntOption(option C.zlink_pub_option_t) (int, error) {
	var raw C.int
	size := C.size_t(C.sizeof_int)
	nativeResult0, nativeErr0 := C.zlink_get_pub_option(s.raw(), option, unsafe.Pointer(&raw), &size)
	if err := configErrorFromCall(nativeResult0, nativeErr0); err != nil {
		return 0, err
	}
	return int(raw), nil
}

func (s *connectionSocket) setPubRoutingIDOption(option C.zlink_pub_option_t, id RoutingID) error {
	raw := routingIDToC(id)
	nativeResult1, nativeErr1 := C.zlink_set_pub_option(s.raw(), option, routingIDPointer(&raw), C.size_t(raw.size))
	return configErrorFromCall(nativeResult1, nativeErr1)
}

func (s *connectionSocket) setPubBytesOption(option C.zlink_pub_option_t, value []byte) error {
	var ptr unsafe.Pointer
	if len(value) > 0 {
		ptr = unsafe.Pointer(&value[0])
	}
	nativeResult2, nativeErr2 := C.zlink_set_pub_option(s.raw(), option, ptr, C.size_t(len(value)))
	return configErrorFromCall(nativeResult2, nativeErr2)
}

func (s *connectionSocket) getPubBytesOption(option C.zlink_pub_option_t, capHint int) ([]byte, error) {
	if capHint <= 0 {
		capHint = 256
	}
	buf := make([]byte, capHint)
	size := C.size_t(len(buf))
	nativeResult3, nativeErr3 := C.zlink_get_pub_option(s.raw(), option, unsafe.Pointer(&buf[0]), &size)
	if err := configErrorFromCall(nativeResult3, nativeErr3); err != nil {
		return nil, err
	}
	return buf[:int(size)], nil
}

func (s *connectionSocket) getSubIntOption(option C.zlink_sub_option_t) (int, error) {
	var raw C.int
	size := C.size_t(C.sizeof_int)
	nativeResult4, nativeErr4 := C.zlink_get_sub_option(s.raw(), option, unsafe.Pointer(&raw), &size)
	if err := configErrorFromCall(nativeResult4, nativeErr4); err != nil {
		return 0, err
	}
	return int(raw), nil
}

func (s *connectionSocket) SetTLSServer(certPath string, keyPath string, requireClientCert bool) error {
	if s == nil || s.socketCore.isClosed() {
		return stateError("socket is closed")
	}
	return setTLSServer(s.raw(), certPath, keyPath, requireClientCert)
}

func (s *connectionSocket) SetTLSClient(caCertPath string, hostname string, trustSystem bool) error {
	if s == nil || s.socketCore.isClosed() {
		return stateError("socket is closed")
	}
	return setTLSClient(s.raw(), caCertPath, hostname, trustSystem)
}
