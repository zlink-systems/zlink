// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"
*/
import "C"

import "unsafe"

func reusableTopicBuffer(buffer []byte) []byte {
	if cap(buffer) < initialTopicBufferSize {
		return make([]byte, initialTopicBufferSize)
	}
	return buffer[:cap(buffer)]
}

func recvTopicWithRetry(
	buffer *[]byte,
	call func(*C.char, C.size_t, *C.size_t) (C.zlink_recv_result_t, error),
) (C.size_t, C.zlink_recv_result_t, error) {
	for {
		capacity := C.size_t(len(*buffer))
		topicLen := capacity
		result, cerr := call((*C.char)(unsafe.Pointer(&(*buffer)[0])), capacity, &topicLen)
		if result != C.ZLINK_RECV_BUFFER_TOO_SMALL || topicLen <= capacity {
			return topicLen, result, cerr
		}

		required := int(topicLen)
		if cap(*buffer) >= required {
			*buffer = (*buffer)[:required]
		} else {
			*buffer = make([]byte, required)
		}
	}
}

func recvTopicMessageInto(
	out *TopicMessage,
	call func(**C.zlink_routing_id_t, *C.char, C.size_t, *C.size_t, *C.zlink_msg_t, C.size_t, *C.size_t, C.zlink_recv_flags_t) (C.zlink_recv_result_t, error),
	flags RecvFlags,
) error {
	var sourceRID *C.zlink_routing_id_t
	topicBuf := reusableTopicBuffer(out.topicBuf)
	var topicLen C.size_t
	reuse := out.parts
	_ = out.Close()
	parts, err := recvMultipart(&out.nativeParts, reuse, flags, func(native *C.zlink_msg_t, capacity C.size_t, count *C.size_t, recvFlags C.zlink_recv_flags_t) (C.zlink_recv_result_t, error) {
		var result C.zlink_recv_result_t
		var cerr error
		topicLen, result, cerr = recvTopicWithRetry(&topicBuf, func(topic *C.char, topicCapacity C.size_t, required *C.size_t) (C.zlink_recv_result_t, error) {
			return call(&sourceRID, topic, topicCapacity, required, native, capacity, count, recvFlags)
		})
		return result, cerr
	})
	out.topicBuf = topicBuf
	if err != nil {
		return err
	}
	out.routingID = routingIDFromCPtr(sourceRID)
	out.topic = string(topicBuf[:int(topicLen)])
	out.parts = parts
	return nil
}

func recvSubscriptionEventInto(
	out *SubscriptionEvent,
	call func(*C.zlink_routing_id_t, *C.int, *C.char, C.size_t, *C.size_t, C.zlink_recv_flags_t) (C.zlink_recv_result_t, error),
	flags RecvFlags,
) error {
	if out == nil {
		return &RecvError{Result: RecvInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	var rid C.zlink_routing_id_t
	var subscribed C.int
	topicBuf := reusableTopicBuffer(out.topicBuf)
	topicLen, result, cerr := recvTopicWithRetry(&topicBuf, func(topic *C.char, topicCapacity C.size_t, required *C.size_t) (C.zlink_recv_result_t, error) {
		return call(&rid, &subscribed, topic, topicCapacity, required, C.zlink_recv_flags_t(flags))
	})
	out.topicBuf = topicBuf
	if err := recvErrorFromCall(result, cerr); err != nil {
		return err
	}
	out.routingID = routingIDFromC(rid)
	out.subscribed = subscribed != 0
	out.topic = string(topicBuf[:int(topicLen)])
	return nil
}
