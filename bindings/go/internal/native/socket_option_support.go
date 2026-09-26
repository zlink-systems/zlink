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

func setNativeOption(raw unsafe.Pointer, closed bool, option C.zlink_option_t, ptr unsafe.Pointer, size C.size_t) error {
	if raw == nil || closed {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	rc, cerr := C.zlink_set_option(raw, option, ptr, size)
	return configErrorFromCall(rc, cerr)
}

func setNativeIntOption(raw unsafe.Pointer, closed bool, option C.zlink_option_t, value int32) error {
	return setNativeOption(raw, closed, option, unsafe.Pointer(&value), C.size_t(C.sizeof_int))
}

func setNativeDurationOption(raw unsafe.Pointer, closed bool, option C.zlink_option_t, value time.Duration) error {
	ms, err := durationToMillis(value)
	if err != nil {
		return err
	}
	return setNativeIntOption(raw, closed, option, ms)
}

func setNativeReceiveFlowState(raw unsafe.Pointer, closed bool, value ReceiveFlowState) error {
	if raw == nil || closed {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	rc, cerr := C.zlink_socket_set_receive_flow_state(raw, C.zlink_receive_flow_state_t(value))
	return configErrorFromCall(rc, cerr)
}

func setNativePubBoolOption(raw unsafe.Pointer, closed bool, option C.zlink_pub_option_t, value bool) error {
	if raw == nil || closed {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	var rawValue C.int
	if value {
		rawValue = 1
	}
	rc, cerr := C.zlink_set_pub_option(raw, option, unsafe.Pointer(&rawValue), C.size_t(C.sizeof_int))
	return configErrorFromCall(rc, cerr)
}

func getNativePubBoolOption(raw unsafe.Pointer, closed bool, option C.zlink_pub_option_t) (bool, error) {
	if raw == nil || closed {
		return false, &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	var rawValue C.int
	size := C.size_t(C.sizeof_int)
	rc, cerr := C.zlink_get_pub_option(raw, option, unsafe.Pointer(&rawValue), &size)
	if err := configErrorFromCall(rc, cerr); err != nil {
		return false, err
	}
	return rawValue != 0, nil
}
