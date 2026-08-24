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
	return s.withCString(filter, func(cstr *C.char) error {
		return configErrorFromResult(C.zlink_set_subscription(s.raw(), cstr))
	})
}

func (s *subscribeSocket) UnsetSubscription(filter string) error {
	return s.withCString(filter, func(cstr *C.char) error {
		return configErrorFromResult(C.zlink_unset_subscription(s.raw(), cstr))
	})
}

func (s *subscribeSocket) Subscribe(out *TopicMessage, flags RecvFlags) (bool, error) {
	if out == nil {
		return false, &RecvError{Result: RecvInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	err := recvTopicMessageInto(out, func(rid **C.zlink_routing_id_t, topic *C.char, topicLen *C.size_t, part *C.zlink_msg_t, hasMore *C.zlink_part_flag_t, recvFlags C.zlink_recv_flags_t) error {
		return recvErrorFromResult(C.zlink_subscribe_part(s.raw(), rid, topic, recvTopicBufferCap, topicLen, part, hasMore, recvFlags))
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
	err := recvSubscriptionEventInto(out, func(rid *C.zlink_routing_id_t, subscribed *C.int, topic *C.char, topicLen *C.size_t, recvFlags C.zlink_recv_flags_t) error {
		var sourceRID *C.zlink_routing_id_t
		if err := recvErrorFromResult(C.zlink_xpub_recv_part(s.raw(), &sourceRID, subscribed, topic, recvTopicBufferCap, topicLen, recvFlags)); err != nil {
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
