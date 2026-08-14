// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"
*/
import "C"

import "unsafe"

type retainedMultipartRecvFunc func(
	*C.zlink_msg_t,
	**C.zlink_hwm_budget_lease_t,
	*C.zlink_part_flag_t,
	C.zlink_recv_flags_t,
) error

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

// recvMultipartRetained intentionally leaves the ordinary receive loop
// unchanged. The owner exists before the first dequeue and adopts each native
// lease before any Go message wrapping that can fail.
func recvMultipartRetained(
	reuse []*Message,
	flags RecvFlags,
	recv retainedMultipartRecvFunc,
) ([]*Message, *hwmBudgetLeaseOwner, error) {
	if reuse == nil {
		reuse = make([]*Message, 0, 1)
	}
	parts := reuse[:0]
	owner := newHwmBudgetLeaseOwner()
	recvFlags := C.zlink_recv_flags_t(flags)
	for {
		// Make every Go allocation needed to own the next result part before
		// Core can atomically dequeue that part and transfer its credit.
		owner.reserveOne()
		if len(parts) == cap(parts) {
			nextCapacity := cap(parts) * 2
			if nextCapacity == 0 {
				nextCapacity = 1
			}
			grown := make([]*Message, len(parts), nextCapacity)
			copy(grown, parts)
			parts = grown
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

		var part C.zlink_msg_t
		if err := configErrorFromResult(C.zlink_msg_init(&part)); err != nil {
			closeMessageSlice(parts)
			owner.release()
			return nil, nil, err
		}

		var lease *C.zlink_hwm_budget_lease_t
		var hasMore C.zlink_part_flag_t
		if err := recv(&part, &lease, &hasMore, recvFlags); err != nil {
			owner.adopt(lease)
			_ = configErrorFromResult(C.zlink_msg_close(&part))
			closeMessageSlice(parts)
			owner.release()
			return nil, nil, err
		}
		owner.adopt(lease)

		if err := configErrorFromResult(C.zlink_msg_adopt(&msg.msg, &part)); err != nil {
			_ = configErrorFromResult(C.zlink_msg_close(&part))
			closeMessageSlice(parts)
			owner.release()
			return nil, nil, err
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
	return parts, owner, nil
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
