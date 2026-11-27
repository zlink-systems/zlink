// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"
*/
import "C"

import "unsafe"

type subscribeSocket struct {
	*connectionSocket
}

func (s *subscribeSocket) SetSubscription(filter string) error {
	handle := s.raw()
	if handle == nil {
		return newConfigError(ConfigInvalidHandle, int(C.EFAULT))
	}
	return s.withCString(filter, func(cstr *C.char) error {
		rc80, errno80 := C.zlink_set_subscription(handle, cstr)
		return configErrorFromCall(rc80, errno80)
	})
}

func (s *subscribeSocket) UnsetSubscription(filter string) error {
	handle := s.raw()
	if handle == nil {
		return newConfigError(ConfigInvalidHandle, int(C.EFAULT))
	}
	return s.withCString(filter, func(cstr *C.char) error {
		rc81, errno81 := C.zlink_unset_subscription(handle, cstr)
		return configErrorFromCall(rc81, errno81)
	})
}

// SubscriptionAt returns the filter at index. The second result reports
// whether the index has an entry.
func (s *subscribeSocket) SubscriptionAt(index int) (Subscription, bool, error) {
	if index < 0 {
		return Subscription{}, false, newConfigError(ConfigInvalidArgument, int(C.EINVAL))
	}
	handle := s.raw()
	buffer := make([]byte, defaultOptionBufferSize)
	size := C.size_t(len(buffer))
	var isPattern C.int
	rc, errno := C.zlink_subscription_at(handle, C.size_t(index), (*C.char)(unsafe.Pointer(unsafe.SliceData(buffer))), &size, &isPattern)
	if rc == C.ZLINK_CONFIG_BUFFER_TOO_SMALL {
		buffer = make([]byte, int(size))
		rc, errno = C.zlink_subscription_at(handle, C.size_t(index), (*C.char)(unsafe.Pointer(unsafe.SliceData(buffer))), &size, &isPattern)
	}
	if rc == C.ZLINK_CONFIG_NOT_FOUND {
		return Subscription{}, false, nil
	}
	if err := configErrorFromCall(rc, errno); err != nil {
		return Subscription{}, false, err
	}
	return Subscription{Filter: string(buffer[:int(size)]), Pattern: isPattern != 0}, true, nil
}

func (s *subscribeSocket) Subscribe(out *TopicMessage, flags RecvFlags) (bool, error) {
	if out == nil {
		return false, newRecvError(RecvInvalidHandle, int(C.EFAULT))
	}
	handle := s.raw()
	if handle == nil {
		return false, newRecvError(RecvInvalidHandle, int(C.EFAULT))
	}
	err := recvTopicMessageInto(out, func(rid **C.zlink_routing_id_t, topic *C.char, topicCapacity C.size_t, topicLen *C.size_t, parts *C.zlink_msg_t, capacity C.size_t, count *C.size_t, recvFlags C.zlink_recv_flags_t) (C.zlink_recv_result_t, error) {
		result, cerr := C.zlink_subscribe(handle, rid, topic, topicCapacity, topicLen, parts, capacity, count, recvFlags)
		return result, cerr
	}, flags)
	if err != nil {
		if isNoData(err) {
			return false, nil
		}
		return false, err
	}
	return true, nil
}

type xpubSubscribeSocket struct {
	*publishSocket
}

func (s *xpubSubscribeSocket) ReceiveSubscriptionEvent(out *SubscriptionEvent, flags RecvFlags) (bool, error) {
	if out == nil {
		return false, newRecvError(RecvInvalidHandle, int(C.EFAULT))
	}
	handle := s.raw()
	if handle == nil {
		return false, newRecvError(RecvInvalidHandle, int(C.EFAULT))
	}
	err := recvSubscriptionEventInto(out, func(rid *C.zlink_routing_id_t, subscribed *C.int, topic *C.char, topicCapacity C.size_t, topicLen *C.size_t, recvFlags C.zlink_recv_flags_t) (C.zlink_recv_result_t, error) {
		var sourceRID *C.zlink_routing_id_t
		rc82, errno82 := C.zlink_xpub_recv(handle, &sourceRID, subscribed, topic, topicCapacity, topicLen, recvFlags)
		if rc82 != C.ZLINK_RECV_OK {
			return rc82, errno82
		}
		if sourceRID != nil {
			*rid = *sourceRID
		} else {
			*rid = C.zlink_routing_id_t{}
		}
		return rc82, errno82
	}, flags)
	if err != nil {
		if isNoData(err) {
			return false, nil
		}
		return false, err
	}
	return true, nil
}
