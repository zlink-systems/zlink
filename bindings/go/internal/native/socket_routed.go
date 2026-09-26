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
	handle := s.raw()
	if handle == nil {
		return &SubmitError{Result: SubmitInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	target := rid.toC()
	return submitMultipartFromClones(parts, true, func(native *C.zlink_msg_t, count C.size_t) error {
		rc79, errno79 := C.zlink_reply(
			handle, &target, C.zlink_reply_token_t(token.value), native, count)
		return submitErrorFromCall(rc79, errno79)
	})
}

func (s *routedSocket) recvInto(out *Received, flags RecvFlags) error {
	handle := s.raw()
	if handle == nil {
		return &RecvError{Result: RecvInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	reuse := out.beginReceive()
	var sourceRID *C.zlink_routing_id_t
	var replyToken C.zlink_reply_token_t
	parts, err := recvMultipart(&out.nativeParts, reuse, flags, func(native *C.zlink_msg_t, capacity C.size_t, count *C.size_t, recvFlags C.zlink_recv_flags_t) (C.zlink_recv_result_t, error) {
		result, cerr := C.zlink_router_recv(
			handle,
			&sourceRID,
			&replyToken,
			native,
			capacity,
			count,
			recvFlags,
		)
		return result, cerr
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
	var send func(context.Context, []sendBuilderPart) (SendSubmission, error)
	if routingID.Size() > 0 {
		send = func(ctx context.Context, builderParts []sendBuilderPart) (SendSubmission, error) {
			return submitManagedSend(ctx, s.socketCore, &routingID, builderParts)
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
	return newSendBuilder(func(ctx context.Context, parts []sendBuilderPart) (SendSubmission, error) {
		return submitManagedSend(ctx, s.socketCore, &target, parts)
	})
}

func (s *RouterSocket) Request(peerRID RoutingID) RequestOp {
	return newRequestBuilder(func(ctx context.Context, parts []requestBuilderPart, timeout time.Duration) (RequestSubmission, error) {
		return submitCompletionRequest(ctx, s.socketCore, &peerRID, timeout, parts)
	})
}

func (s *RouterSocket) Reply(rid RoutingID, token ReplyToken) ReplyOp {
	return newReplyBuilder(func(parts []*Message) error {
		return s.reply(rid, token, parts...)
	})
}
