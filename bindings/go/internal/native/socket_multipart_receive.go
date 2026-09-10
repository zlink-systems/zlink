// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"
*/
import "C"

import (
	"runtime"
	"unsafe"
)

const nativeRecvInitialCapacity = 8

func recvMultipart(nativeBuffer *[]C.zlink_msg_t, reuse []*Message, flags RecvFlags, recv multipartRecvFunc) ([]*Message, error) {
	// Keep errno and a possible ENOBUFS retry on the same native thread.
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	if len(*nativeBuffer) == 0 {
		*nativeBuffer = make([]C.zlink_msg_t, nativeRecvInitialCapacity)
	}
	native := *nativeBuffer
	for {
		var partCount C.size_t
		result := recv(
			&native[0], C.size_t(len(native)), &partCount,
			C.zlink_recv_flags_t(flags),
		)
		if result == C.ZLINK_RECV_BUFFER_TOO_SMALL {
			if partCount <= C.size_t(len(native)) {
				return nil, &RecvError{Result: RecvInternalError, nativeErrno: int(C.EPROTO)}
			}
			required := int(partCount)
			if required <= 0 {
				return nil, &RecvError{Result: RecvInternalError, nativeErrno: int(C.EOVERFLOW)}
			}
			if cap(native) >= required {
				native = native[:required]
			} else {
				native = make([]C.zlink_msg_t, required)
			}
			*nativeBuffer = native
			continue
		}
		if err := recvErrorFromResult(result); err != nil {
			return nil, err
		}
		if partCount == 0 || partCount > C.size_t(len(native)) {
			return nil, &RecvError{Result: RecvInternalError, nativeErrno: int(C.EPROTO)}
		}
		return adoptReceivedParts(native, int(partCount), reuse)
	}
}

func adoptReceivedParts(native []C.zlink_msg_t, count int, reuse []*Message) ([]*Message, error) {
	parts := reuse[:0]
	for i := 0; i < count; i++ {
		var msg *Message
		if i < len(reuse) {
			msg = reuse[i]
		}
		if msg == nil {
			msg = &Message{}
		}
		if err := configErrorFromResult(C.zlink_msg_init(&msg.msg)); err != nil {
			closeMessageSlice(parts)
			closeNativeMultipart(native, count)
			return nil, err
		}
		if err := configErrorFromResult(C.zlink_msg_move(&msg.msg, &native[i])); err != nil {
			_ = configErrorFromResult(C.zlink_msg_close(&msg.msg))
			msg.closed = true
			closeMessageSlice(parts)
			closeNativeMultipart(native, count)
			return nil, err
		}
		msg.closed = false
		parts = append(parts, msg)
	}
	closeNativeMultipart(native, count)
	for i := count; i < len(reuse); i++ {
		reuse[i] = nil
	}
	return parts, nil
}

func takeParts(ptr *C.zlink_msg_t, partCount C.size_t) ([]*Message, error) {
	count := int(partCount)
	if count == 0 || ptr == nil {
		return nil, nil
	}
	raw := unsafe.Slice(ptr, count)
	parts := make([]*Message, 0, count)
	for i := 0; i < count; i++ {
		msg := &Message{}
		if err := configErrorFromResult(C.zlink_msg_init(&msg.msg)); err != nil {
			closeMessageSlice(parts)
			C.zlink_multipart_close(ptr, partCount)
			return nil, err
		}
		if err := configErrorFromResult(C.zlink_msg_move(&msg.msg, &raw[i])); err != nil {
			_ = msg.Close()
			closeMessageSlice(parts)
			C.zlink_multipart_close(ptr, partCount)
			return nil, err
		}
		parts = append(parts, msg)
	}
	C.zlink_multipart_close(ptr, partCount)
	return parts, nil
}

func discardParts(ptr *C.zlink_msg_t, partCount C.size_t) {
	if ptr == nil || partCount == 0 {
		return
	}
	C.zlink_multipart_close(ptr, partCount)
}
