// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include "zlink.h"
*/
import "C"

func prepareRequestMultipart(parts []requestBuilderPart) (*preparedMultipart, error) {
	if len(parts) == 0 {
		return nil, &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	native := make([]C.zlink_msg_t, len(parts))
	for i, part := range parts {
		if part.bytes {
			if err := initNativeMessageFromBytes(&native[i], part.data); err != nil {
				closeNativeMultipart(native, i)
				return nil, err
			}
			continue
		}
		if part.message == nil {
			closeNativeMultipart(native, i)
			return nil, &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
		}
		if part.message.closed {
			closeNativeMultipart(native, i)
			return nil, &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
		}
		if err := configErrorFromResult(C.zlink_msg_init(&native[i])); err != nil {
			closeNativeMultipart(native, i)
			return nil, err
		}
		if err := configErrorFromResult(C.zlink_msg_copy(&native[i], &part.message.msg)); err != nil {
			closeNativeMultipart(native, i+1)
			return nil, err
		}
	}
	return &preparedMultipart{native: native}, nil
}

func consumeRequestBuilderMessages(parts []requestBuilderPart) {
	for _, part := range parts {
		if part.message != nil {
			_ = part.message.Close()
		}
	}
}
