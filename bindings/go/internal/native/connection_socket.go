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

func (s *connectionSocket) completionOwner() *socketCore {
	if s == nil {
		return nil
	}
	return s.socketCore
}

func (s *connectionSocket) SetSendHighWaterMark(value int) error {
	return s.setHwmOption(C.ZLINK_OPT_SNDHWM, value)
}

func (s *connectionSocket) SendHighWaterMark() (int, error) {
	return s.getHwmOption(C.ZLINK_OPT_SNDHWM)
}

func (s *connectionSocket) SetReceiveHighWaterMark(value int) error {
	return s.setHwmOption(C.ZLINK_OPT_RCVHWM, value)
}

func (s *connectionSocket) ReceiveHighWaterMark() (int, error) {
	return s.getHwmOption(C.ZLINK_OPT_RCVHWM)
}

func (s *connectionSocket) SetLinger(value time.Duration) error {
	return s.setDurationOption(C.ZLINK_OPT_LINGER, value)
}

func (s *connectionSocket) SetReceiveTimeout(value time.Duration) error {
	return s.setDurationOption(C.ZLINK_OPT_RCVTIMEO, value)
}

func (s *connectionSocket) SetSendTimeout(value time.Duration) error {
	return s.setDurationOption(C.ZLINK_OPT_SNDTIMEO, value)
}

func (s *connectionSocket) SetTCPKeepalive(value bool) error {
	return s.setBoolOption(C.ZLINK_OPT_TCP_KEEPALIVE, value)
}

func (s *connectionSocket) SetTCPNoDelay(value bool) error {
	return s.setBoolOption(C.ZLINK_OPT_TCP_NODELAY, value)
}

func (s *connectionSocket) SetIPv6(value bool) error {
	return s.setBoolOption(C.ZLINK_OPT_IPV6, value)
}

func (s *connectionSocket) SetRidDuplicatePolicy(value int) error {
	return s.setIntOption(C.ZLINK_OPT_RID_DUPLICATE_POLICY, int32(value))
}

func (s *connectionSocket) RidDuplicatePolicy() (int, error) {
	value, err := s.getIntOption(C.ZLINK_OPT_RID_DUPLICATE_POLICY)
	return int(value), err
}

func (s *connectionSocket) SetSubmitRetryMode(value SubmitRetryMode) error {
	return s.setIntOption(C.ZLINK_OPT_SUBMIT_RETRY_MODE, int32(value))
}

func (s *connectionSocket) SubmitRetryMode() (SubmitRetryMode, error) {
	value, err := s.getIntOption(C.ZLINK_OPT_SUBMIT_RETRY_MODE)
	return SubmitRetryMode(value), err
}

func (s *connectionSocket) SetSubmitRetryTimeout(value time.Duration) error {
	return s.setDurationOption(C.ZLINK_OPT_SUBMIT_RETRY_TIMEOUT, value)
}

func (s *connectionSocket) SubmitRetryTimeout() (time.Duration, error) {
	return s.getDurationOption(C.ZLINK_OPT_SUBMIT_RETRY_TIMEOUT)
}

func (s *connectionSocket) SetSubmitRetryAttempts(value int) error {
	return s.setIntOption(C.ZLINK_OPT_SUBMIT_RETRY_ATTEMPTS, int32(value))
}

func (s *connectionSocket) SubmitRetryAttempts() (int, error) {
	value, err := s.getIntOption(C.ZLINK_OPT_SUBMIT_RETRY_ATTEMPTS)
	return int(value), err
}

func (s *connectionSocket) CommonOptions() *CommonSocketOptions {
	return &CommonSocketOptions{socket: s}
}

func (s *connectionSocket) LastEndpoint() (string, error) {
	return s.getStringOption(C.ZLINK_OPT_LAST_ENDPOINT, 256)
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
	if err := configErrorFromResult(C.zlink_get_pub_option(s.raw(), option, unsafe.Pointer(&raw), &size)); err != nil {
		return 0, err
	}
	return int(raw), nil
}

func (s *connectionSocket) setPubRoutingIDOption(option C.zlink_pub_option_t, id RoutingID) error {
	raw := id.toC()
	return configErrorFromResult(C.zlink_set_pub_option(s.raw(), option, routingIDPointer(&raw), C.size_t(raw.size)))
}

func (s *connectionSocket) setPubBytesOption(option C.zlink_pub_option_t, value []byte) error {
	var ptr unsafe.Pointer
	if len(value) > 0 {
		ptr = unsafe.Pointer(&value[0])
	}
	return configErrorFromResult(C.zlink_set_pub_option(s.raw(), option, ptr, C.size_t(len(value))))
}

func (s *connectionSocket) getPubBytesOption(option C.zlink_pub_option_t, capHint int) ([]byte, error) {
	if capHint <= 0 {
		capHint = 256
	}
	buf := make([]byte, capHint)
	size := C.size_t(len(buf))
	if err := configErrorFromResult(C.zlink_get_pub_option(s.raw(), option, unsafe.Pointer(&buf[0]), &size)); err != nil {
		return nil, err
	}
	return buf[:int(size)], nil
}

func (s *connectionSocket) getSubIntOption(option C.zlink_sub_option_t) (int, error) {
	var raw C.int
	size := C.size_t(C.sizeof_int)
	if err := configErrorFromResult(C.zlink_get_sub_option(s.raw(), option, unsafe.Pointer(&raw), &size)); err != nil {
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
