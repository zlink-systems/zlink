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
	return configErrorFromResult(ConfigResult(C.zlink_set_option(raw, option, ptr, size)))
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

func setNativePubBoolOption(raw unsafe.Pointer, closed bool, option C.zlink_pub_option_t, value bool) error {
	if raw == nil || closed {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	var rawValue C.int
	if value {
		rawValue = 1
	}
	return configErrorFromResult(ConfigResult(C.zlink_set_pub_option(raw, option, unsafe.Pointer(&rawValue), C.size_t(C.sizeof_int))))
}

func getNativePubBoolOption(raw unsafe.Pointer, closed bool, option C.zlink_pub_option_t) (bool, error) {
	if raw == nil || closed {
		return false, &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	var rawValue C.int
	size := C.size_t(C.sizeof_int)
	if err := configErrorFromResult(ConfigResult(C.zlink_get_pub_option(raw, option, unsafe.Pointer(&rawValue), &size))); err != nil {
		return false, err
	}
	return rawValue != 0, nil
}
