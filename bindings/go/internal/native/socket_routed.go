// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"
*/
import "C"

import (
	"context"
	"time"
)

type routedSocket struct {
	*connectionSocket
	replyOwner *replyTokenOwner
}

func (s *routedSocket) reply(rid RoutingID, token ReplyToken, parts ...*Message) error {
	if token.owner == nil || token.value == 0 || token.owner != s.replyOwner {
		return &SubmitError{Result: SubmitInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	target := rid.toC()
	return submitMultipartFromClones(parts, true, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
		return submitErrorFromResult(C.zlink_reply_part(
			s.raw(), &target, C.zlink_reply_token_t(token.value), part, partFlag))
	})
}

func (s *routedSocket) recvInto(out *Received, flags RecvFlags) error {
	reuse := out.beginReceive()
	var sourceRID *C.zlink_routing_id_t
	var replyToken C.zlink_reply_token_t
	parts, err := recvMultipart(reuse, flags, func(part *C.zlink_msg_t, hasMore *C.zlink_part_flag_t, recvFlags C.zlink_recv_flags_t) error {
		return recvErrorFromResult(C.zlink_router_recv_part(
			s.raw(),
			&sourceRID,
			&replyToken,
			part,
			hasMore,
			recvFlags,
		))
	})
	if err != nil {
		return err
	}

	s.replaceRoutedReceived(out, routingIDFromCPtr(sourceRID), parts, uint64(replyToken))
	return nil
}

func (s *routedSocket) replaceRoutedReceived(
	out *Received,
	routingID RoutingID,
	parts []*Message,
	tokenValue uint64,
) {
	var token ReplyToken
	var reply func([]*Message) error
	if tokenValue != 0 {
		token = ReplyToken{owner: s.replyOwner, value: tokenValue}
		reply = func(parts []*Message) error { return s.reply(routingID, token, parts...) }
	}
	var send func(context.Context, []sendBuilderPart) error
	if routingID.Size() > 0 {
		send = func(ctx context.Context, builderParts []sendBuilderPart) error {
			return submitCompletionSend(ctx, s.socketCore, &routingID, false, builderParts)
		}
	}
	out.replace(routingID, parts, token, reply, send)
}

func (s *routedSocket) Recv(out *Received, flags RecvFlags) (bool, error) {
	if out == nil {
		return false, &RecvError{Result: RecvInvalidHandle, nativeErrno: int(C.EFAULT)}
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
	return newSendBuilder(func(ctx context.Context, parts []sendBuilderPart) error {
		return submitCompletionSend(ctx, s.socketCore, &target, false, parts)
	})
}

func (s *RouterSocket) Request(peerRID RoutingID) RequestOp {
	return newRequestBuilder(func(ctx context.Context, parts []requestBuilderPart, timeout time.Duration) ([]*Message, error) {
		return submitCompletionRequest(ctx, s.socketCore, &peerRID, timeout, parts)
	})
}

func (s *RouterSocket) Reply(rid RoutingID, token ReplyToken) ReplyOp {
	return newReplyBuilder(func(parts []*Message) error {
		return s.reply(rid, token, parts...)
	})
}
