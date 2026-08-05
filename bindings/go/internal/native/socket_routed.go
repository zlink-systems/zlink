// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"

extern void goZlinkReplyTrampoline(zlink_request_result_t result_, zlink_msg_t *parts_, size_t part_count_, uintptr_t userdata_);
*/
import "C"

import "time"

type routedSocket struct {
	*connectionSocket
}

func (s *routedSocket) OnSendReady(handler func()) error {
	return s.setSendReady(handler)
}

func (s *routedSocket) submitTo(target RoutingID, flags SendFlags, parts ...*Message) (bool, error) {
	rid := target.toC()
	err := submitMultipartFromClones(parts, true, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
		return submitErrorFromResult(C.zlink_send_part_rid(s.raw(), &rid, part, C.zlink_send_flags_t(flags), partFlag))
	})
	return submitBackpressureResult(err)
}

func (s *routedSocket) submitToBuilder(target RoutingID, flags SendFlags, parts []sendBuilderPart) (bool, error) {
	rid := target.toC()
	err := submitMultipartFromBuilderParts(parts, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
		return submitErrorFromResult(C.zlink_send_part_rid(s.raw(), &rid, part, C.zlink_send_flags_t(flags), partFlag))
	})
	return submitBackpressureResult(err)
}

func (s *routedSocket) reply(rid RoutingID, requestSeq uint64, flags SendFlags, parts ...*Message) error {
	if err := validateReplyFlags(flags); err != nil {
		return err
	}
	target := rid.toC()
	return submitMultipartFromClones(parts, true, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
		return submitErrorFromResult(C.zlink_router_reply_part(s.raw(), &target, C.uint64_t(requestSeq), part, partFlag))
	})
}

func (s *routedSocket) recvInto(out *Received, flags RecvFlags) error {
	reuse := out.beginReceive()
	var sourceRID *C.zlink_routing_id_t
	var requestSeq C.uint64_t
	parts, err := recvMultipart(reuse, flags, func(part *C.zlink_msg_t, hasMore *C.zlink_part_flag_t, recvFlags C.zlink_recv_flags_t) error {
		return recvErrorFromResult(C.zlink_router_recv_part(
			s.raw(),
			&sourceRID,
			&requestSeq,
			part,
			hasMore,
			recvFlags,
		))
	})
	if err != nil {
		return err
	}

	routingID := routingIDFromCPtr(sourceRID)
	seq := uint64(requestSeq)
	hasSeq := seq != 0
	var reply func(SendFlags, []*Message) error
	if hasSeq {
		reply = receivedReplyToRouter(s.reply, routingID, seq)
	}
	var send func(SendFlags, []sendBuilderPart) (bool, error)
	if routingID.Size() > 0 {
		send = func(sendFlags SendFlags, builderParts []sendBuilderPart) (bool, error) {
			return s.submitToBuilder(routingID, sendFlags, builderParts)
		}
	}
	out.replace(routingID, parts, seq, hasSeq, reply, send)
	return nil
}

func (s *routedSocket) Recv(out *Received, flags RecvFlags) (bool, error) {
	if out == nil {
		return false, &RecvError{Result: RecvInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	if s.connectionSocket.hasReceiveHandler() {
		return false, &RecvError{Result: RecvBusy, nativeErrno: int(C.EBUSY)}
	}
	if err := s.recvInto(out, flags); err != nil {
		if isNoData(err) {
			return false, nil
		}
		return false, err
	}
	return true, nil
}
func (s *RouterSocket) SendTo(target RoutingID) SendOp {
	return newSendBuilder(func(parts []sendBuilderPart, flags SendFlags) error {
		rid := target.toC()
		return submitMultipartFromBuilderParts(parts, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
			return submitErrorFromResult(C.zlink_send_part_rid(
				s.raw(),
				&rid,
				part,
				C.zlink_send_flags_t(flags),
				partFlag,
			))
		})
	})
}

func (s *RouterSocket) Request(peerRID RoutingID) RequestOp {
	return newRequestBuilder(func(
		parts []requestBuilderPart,
		flags SendFlags,
		timeout time.Duration,
		callback RequestReplyCallback,
	) error {
		if callback == nil {
			return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
		}
		_, err := startRouterRequest(s, peerRID, flags, timeout, parts, callback)
		if err != nil {
			return err
		}
		return nil
	})
}

func (s *RouterSocket) Reply(rid RoutingID, requestSeq uint64) ReplyOp {
	return newReplyBuilder(func(parts []*Message, flags SendFlags) error {
		return s.reply(rid, requestSeq, flags, parts...)
	})
}
