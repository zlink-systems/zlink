// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"
*/
import "C"

import "unsafe"

func recvMultipart(reuse []*Message, flags RecvFlags, recv multipartRecvFunc) ([]*Message, error) {
	if reuse == nil {
		reuse = make([]*Message, 0, 1)
	}
	parts := reuse[:0]
	recvFlags := C.zlink_recv_flags_t(flags)
	for {
		var part C.zlink_msg_t
		if err := configErrorFromResult(C.zlink_msg_init(&part)); err != nil {
			closeMessageSlice(parts)
			return nil, err
		}

		var hasMore C.zlink_part_flag_t
		if err := recv(&part, &hasMore, recvFlags); err != nil {
			_ = configErrorFromResult(C.zlink_msg_close(&part))
			closeMessageSlice(parts)
			return nil, err
		}

		var msg *Message
		if len(parts) < len(reuse) {
			msg = reuse[len(parts)]
			if msg == nil || msg.closed {
				msg = &Message{}
				reuse[len(parts)] = msg
			}
		} else {
			msg = &Message{}
		}
		if err := configErrorFromResult(C.zlink_msg_adopt(&msg.msg, &part)); err != nil {
			_ = configErrorFromResult(C.zlink_msg_close(&part))
			closeMessageSlice(parts)
			return nil, err
		}
		msg.closed = false
		parts = append(parts, msg)

		if hasMore == 0 {
			break
		}
		recvFlags = C.zlink_recv_flags_t(C.ZLINK_DONTWAIT)
	}
	for i := len(parts); i < len(reuse); i++ {
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
