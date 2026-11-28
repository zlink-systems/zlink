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
	"zlink.systems/zlink/contracts"
)

type routedSocket struct {
	*connectionSocket
	replyOwner *replyTokenOwner
}

func (s *routedSocket) reply(rid RoutingID, token ReplyToken, parts ...*Message) error {
	if token.owner == nil || token.value == 0 || token.owner != s.replyOwner {
		return newSubmitError(SubmitInvalidArgument, int(C.EINVAL))
	}
	return s.submitReply(rid, token.value, parts...)
}

func (s *socketCore) submitReply(rid RoutingID, token uint64, parts ...*Message) error {
	handle := s.raw()
	if handle == nil {
		return newSubmitError(SubmitInvalidHandle, int(C.EFAULT))
	}
	target := routingIDToC(rid)
	return submitMultipartFromClones(parts, true, func(native *C.zlink_msg_t, count C.size_t) error {
		rc79, errno79 := C.zlink_reply(
			handle, &target, C.zlink_reply_token_t(token), native, count)
		return submitErrorFromCall(rc79, errno79)
	})
}

func (s *routedSocket) recvInto(out *Received, flags RecvFlags) error {
	handle := s.raw()
	if handle == nil {
		return newRecvError(RecvInvalidHandle, int(C.EFAULT))
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

	// Core reports the route generation of the record returned by the last
	// successful zlink_router_recv; read it before any other data receive.
	routeGeneration := uint64(C.zlink_router_recv_route_generation(s.raw()))
	s.replaceRoutedReceived(out, routingIDFromCPtr(sourceRID), parts, uint64(replyToken), routeGeneration)
	return nil
}

func (s *routedSocket) replaceRoutedReceived(
	out *Received,
	routingID RoutingID,
	parts []*Message,
	tokenValue uint64,
	routeGeneration uint64,
) {
	var token ReplyToken
	if tokenValue != 0 {
		token = ReplyToken{owner: s.replyOwner, value: tokenValue}
	}
	out.replace(routingID, parts, token, s.socketCore, routeGeneration)
}

func (s *routedSocket) Recv(out *Received, flags RecvFlags) (bool, error) {
	if out == nil {
		return false, newRecvError(RecvInvalidHandle, int(C.EFAULT))
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

type RouterRoute = contracts.RouterRoute

// RoutesSnapshot returns every selected route atomically, one row per
// routing id. A successful call clears PollRoute readiness unless a later
// change raced with it. Keep one route observer per socket.
func (s *RouterSocket) RoutesSnapshot() ([]RouterRoute, error) {
	const initialCapacity = 16
	native := make([]C.zlink_router_route_t, initialCapacity)
	for {
		var count C.size_t
		rc, cerr := C.zlink_router_routes_snapshot(s.raw(), &native[0], C.size_t(len(native)), &count)
		if ConfigResult(rc) == ConfigBufferTooSmall && int(count) > len(native) {
			// Core keeps POLLROUTE readiness on this result; retry with the
			// count it reported (the set can grow again before the retry).
			native = make([]C.zlink_router_route_t, int(count))
			continue
		}
		if err := configErrorFromCall(rc, cerr); err != nil {
			return nil, err
		}
		routes := make([]RouterRoute, int(count))
		for i := 0; i < int(count); i++ {
			routes[i] = RouterRoute{
				RoutingID:       routingIDFromC(native[i].rid),
				RouteGeneration: uint64(native[i].route_generation),
			}
		}
		return routes, nil
	}
}
