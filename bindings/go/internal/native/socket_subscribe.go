// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"
*/
import "C"

type subscribeSocket struct {
	*connectionSocket
}

func (s *subscribeSocket) SetSubscription(filter string) error {
	handle := s.raw()
	if handle == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	return s.withCString(filter, func(cstr *C.char) error {
		rc80, errno80 := C.zlink_set_subscription(handle, cstr)
		return configErrorFromCall(rc80, errno80)
	})
}

func (s *subscribeSocket) UnsetSubscription(filter string) error {
	handle := s.raw()
	if handle == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	return s.withCString(filter, func(cstr *C.char) error {
		rc81, errno81 := C.zlink_unset_subscription(handle, cstr)
		return configErrorFromCall(rc81, errno81)
	})
}

func (s *subscribeSocket) Subscribe(out *TopicMessage, flags RecvFlags) (bool, error) {
	if out == nil {
		return false, &RecvError{Result: RecvInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	handle := s.raw()
	if handle == nil {
		return false, &RecvError{Result: RecvInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	err := recvTopicMessageInto(out, func(rid **C.zlink_routing_id_t, topic *C.char, topicLen *C.size_t, parts *C.zlink_msg_t, capacity C.size_t, count *C.size_t, recvFlags C.zlink_recv_flags_t) (C.zlink_recv_result_t, error) {
		result, cerr := C.zlink_subscribe(handle, rid, topic, recvTopicBufferCap, topicLen, parts, capacity, count, recvFlags)
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
		return false, &RecvError{Result: RecvInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	handle := s.raw()
	if handle == nil {
		return false, &RecvError{Result: RecvInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	err := recvSubscriptionEventInto(out, func(rid *C.zlink_routing_id_t, subscribed *C.int, topic *C.char, topicLen *C.size_t, recvFlags C.zlink_recv_flags_t) error {
		var sourceRID *C.zlink_routing_id_t
		rc82, errno82 := C.zlink_xpub_recv(handle, &sourceRID, subscribed, topic, recvTopicBufferCap, topicLen, recvFlags)
		if err := recvErrorFromCall(rc82, errno82); err != nil {
			return err
		}
		if sourceRID != nil {
			*rid = *sourceRID
		} else {
			*rid = C.zlink_routing_id_t{}
		}
		return nil
	}, flags)
	if err != nil {
		if isNoData(err) {
			return false, nil
		}
		return false, err
	}
	return true, nil
}
