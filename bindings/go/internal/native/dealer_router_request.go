// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include "zlink.h"

extern void goZlinkReplyTrampoline(zlink_request_result_t result_, zlink_msg_t *parts_, size_t part_count_, uintptr_t userdata_);

static inline int zlink_dealer_request_part_go_local(void *dealer, zlink_msg_t *part, zlink_send_flags_t flags, zlink_part_flag_t part_flag, uint32_t timeout_ms, uintptr_t userdata) {
    return zlink_dealer_request_part(dealer, part, flags, part_flag, timeout_ms, (zlink_reply_handler_fn)goZlinkReplyTrampoline, (void *)userdata);
}

static inline int zlink_router_request_part_go_local(void *router, const zlink_routing_id_t *peer_rid, zlink_msg_t *part, zlink_send_flags_t flags, zlink_part_flag_t part_flag, uint32_t timeout_ms, uintptr_t userdata) {
    return zlink_router_request_part(router, peer_rid, part, flags, part_flag, timeout_ms, (zlink_reply_handler_fn)goZlinkReplyTrampoline, (void *)userdata);
}
*/
import "C"

import (
	"runtime/cgo"
	"time"
)

func startDealerRequest(socket *DealerSocket, flags SendFlags, timeout time.Duration, parts []requestBuilderPart, callback RequestReplyCallback) (*replyCallbackState, error) {
	prepared, err := prepareRequestMultipart(parts)
	if err != nil {
		return nil, err
	}
	state := newReplyCallbackState()
	dispatchRequestCallback(state, socket.socketCore.requestCallbackDispatcher(), callback)
	handle := cgo.NewHandle(state)
	if err := submitPreparedMultipart(prepared, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
		return submitErrorFromResult(C.zlink_dealer_request_part_go_local(
			socket.raw(),
			part,
			C.zlink_send_flags_t(flags),
			partFlag,
			C.uint32_t(requestTimeoutMillis(timeout)),
			C.uintptr_t(handle),
		))
	}); err != nil {
		handle.Delete()
		return nil, err
	}
	consumeRequestBuilderMessages(parts)
	socket.socketCore.startRequestProgress(state)
	return state, nil
}

func startRouterRequest(socket *RouterSocket, routingID RoutingID, flags SendFlags, timeout time.Duration, parts []requestBuilderPart, callback RequestReplyCallback) (*replyCallbackState, error) {
	prepared, err := prepareRequestMultipart(parts)
	if err != nil {
		return nil, err
	}
	state := newReplyCallbackState()
	dispatchRequestCallback(state, socket.socketCore.requestCallbackDispatcher(), callback)
	handle := cgo.NewHandle(state)
	rid := routingID.toC()
	if err := submitPreparedMultipart(prepared, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
		return submitErrorFromResult(C.zlink_router_request_part_go_local(
			socket.raw(),
			&rid,
			part,
			C.zlink_send_flags_t(flags),
			partFlag,
			C.uint32_t(requestTimeoutMillis(timeout)),
			C.uintptr_t(handle),
		))
	}); err != nil {
		handle.Delete()
		return nil, err
	}
	consumeRequestBuilderMessages(parts)
	socket.socketCore.startRequestProgress(state)
	return state, nil
}
