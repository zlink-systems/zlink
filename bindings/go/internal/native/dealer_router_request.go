// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <errno.h>
#include <stdint.h>
#include "zlink.h"

extern void goZlinkReplyTrampoline(zlink_request_result_t result_, zlink_msg_t *parts_, size_t part_count_, uintptr_t userdata_);

static inline int zlink_dealer_request_transport_pair_part_go_local(
    void *dealer_, const zlink_routed_submit_target_t *target_,
    zlink_msg_t *part_, zlink_send_flags_t flags_, zlink_part_flag_t part_flag_,
    uint32_t timeout_ms_, uintptr_t userdata_) {
    zlink_reply_handler_fn handler_ = userdata_ == 0
        ? NULL : (zlink_reply_handler_fn)goZlinkReplyTrampoline;
    return zlink_dealer_request_transport_pair_part(
        dealer_, target_, part_, flags_, part_flag_, timeout_ms_, handler_,
        (void *)userdata_);
}

static inline int zlink_router_request_transport_pair_part_go_local(
    void *router_, const zlink_routing_id_t *peer_rid_,
    uint64_t transport_pair_id_, uint64_t transport_pair_generation_,
    zlink_msg_t *part_, zlink_send_flags_t flags_, zlink_part_flag_t part_flag_,
    uint32_t timeout_ms_, uintptr_t userdata_) {
    zlink_reply_handler_fn handler_ = userdata_ == 0
        ? NULL : (zlink_reply_handler_fn)goZlinkReplyTrampoline;
    return zlink_router_request_transport_pair_part(
        router_, peer_rid_, transport_pair_id_, transport_pair_generation_,
        part_, flags_, part_flag_, timeout_ms_, handler_, (void *)userdata_);
}
*/
import "C"

import (
	"context"
	"errors"
	"runtime/cgo"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

// routedRole distinguishes the two HWM-managed routed socket families. The
// role only selects which Core entry point a submit uses; the binding keeps no
// per-role state.
type routedRole uint8

const (
	routedDealer routedRole = iota
	routedRouter
)

// submitRoutedSend is the synchronous routed-send terminal
// (`RoutedSendSubmitOp.Submit(ctx) error`).
//
// The binding owns no thread, no queue and no retry. The whole HWM wait lives
// inside Core: a blocking `zlink_send_part`/`zlink_send_part_rid` parks in Core
// and resumes on a Core credit signal, bounded by the socket's `SNDTIMEO`
// (`SNDTIMEO=0` is the DONTWAIT contract and returns BACKPRESSURED at once).
//
// ctx owns cancellation and deadlines at the submit boundary only: an already
// cancelled or expired ctx fails before anything reaches the wire, and a ctx
// deadline shorter than the remaining time is refused up front. Once Core has
// taken the record the wait belongs to Core, so cancelling ctx afterwards does
// not interrupt it — bound that wait with `SetSendTimeout`.
func submitRoutedSend(
	ctx context.Context,
	core *socketCore,
	role routedRole,
	routerRID *RoutingID,
	parts []sendBuilderPart,
) error {
	if err := contextError(ctx); err != nil {
		return err
	}
	if core == nil {
		return &SubmitError{Result: SubmitInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	if role == routedRouter && routerRID == nil {
		return &SubmitError{Result: SubmitInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	handle := core.raw()
	if role == routedDealer {
		return submitMultipartFromBuilderParts(parts, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
			return submitErrorFromResult(C.zlink_send_part(
				handle, part, C.ZLINK_SEND_FLAGS_NONE, partFlag))
		})
	}
	rid := routerRID.toC()
	return submitMultipartFromBuilderParts(parts, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
		return submitErrorFromResult(C.zlink_send_part_rid(
			handle, &rid, part, C.ZLINK_SEND_FLAGS_NONE, partFlag))
	})
}

// requestCompletion bridges the Core reply callback to the single completion
// channel returned by `RequestSubmitOp.Submit(ctx)`. It owns no goroutine of
// its own: the reply is delivered on whatever context Core runs the reply
// handler on, and the optional ctx watch uses the runtime's own
// context.AfterFunc.
type requestCompletion struct {
	mu      sync.Mutex
	done    bool
	result  chan RequestReplyCompletion
	stopCtx func() bool
}

func (r *requestCompletion) install(stopCtx func() bool) {
	if stopCtx == nil {
		return
	}
	r.mu.Lock()
	if r.done {
		r.mu.Unlock()
		stopCtx()
		return
	}
	r.stopCtx = stopCtx
	r.mu.Unlock()
}

func (r *requestCompletion) deliver(completion RequestReplyCompletion) {
	r.mu.Lock()
	if r.done {
		r.mu.Unlock()
		MultipartClose(completion.Parts)
		return
	}
	r.done = true
	stopCtx := r.stopCtx
	r.stopCtx = nil
	r.mu.Unlock()
	if stopCtx != nil {
		stopCtx()
	}
	r.result <- completion
	close(r.result)
}

func (r *requestCompletion) onReply(result requestResult) {
	r.deliver(RequestReplyCompletion{
		Result: result.result,
		Parts:  result.parts,
		Err:    requestCompletionError(result.result),
	})
}

func (r *requestCompletion) fail(err error) {
	r.deliver(requestCompletionFromError(err))
}

// submitRoutedRequest keeps the request terminal asynchronous
// (`Submit(ctx) -> <-chan RequestReplyCompletion`) while the submit itself is
// synchronous, exactly like the C++ reference realignment:
//
//  1. snapshot one exact routed target (a value snapshot, not a credit
//     reservation) so the request and its reply stay on one physical peer,
//  2. hand the record to Core with blocking flags — Core owns the HWM wait,
//  3. arm the reply bridge; Core's reply handler (or its
//     ZLINK_REQUEST_TIMED_OUT) completes the channel.
//
// The binding adds no retry, no deadline thread and no dispatcher. ctx
// cancellation after admission completes the caller's channel early; the late
// Core reply is then dropped and its parts released.
func submitRoutedRequest(
	ctx context.Context,
	core *socketCore,
	role routedRole,
	routerRID *RoutingID,
	timeout time.Duration,
	parts []requestBuilderPart,
) <-chan RequestReplyCompletion {
	result := make(chan RequestReplyCompletion, 1)
	completion := &requestCompletion{result: result}

	if err := contextError(ctx); err != nil {
		completion.fail(err)
		return result
	}
	if core == nil {
		completion.fail(&SubmitError{Result: SubmitInvalidHandle, nativeErrno: int(C.EFAULT)})
		return result
	}
	if role == routedRouter && routerRID == nil {
		completion.fail(&SubmitError{Result: SubmitInvalidArgument, nativeErrno: int(C.EINVAL)})
		return result
	}

	timeoutMillis, err := requestTimeoutMillis(core, role, timeout)
	if err != nil {
		completion.fail(err)
		return result
	}

	target, err := selectRoutedTarget(core, routerRID)
	if err == nil {
		err = submitExactRoutedRequest(
			core.raw(), role, routerRID, &target, parts, timeoutMillis, completion.onReply)
	}
	if err != nil {
		completion.fail(err)
		return result
	}

	if ctx != nil && ctx.Done() != nil {
		completion.install(context.AfterFunc(ctx, func() {
			completion.fail(ctx.Err())
		}))
	}
	return result
}

func selectRoutedTarget(core *socketCore, routerRID *RoutingID) (C.zlink_routed_submit_target_t, error) {
	var target C.zlink_routed_submit_target_t
	var rid C.zlink_routing_id_t
	var ridPointer *C.zlink_routing_id_t
	if routerRID != nil {
		if routerRID.Size() == 0 {
			return target, &SubmitError{Result: SubmitInvalidArgument, nativeErrno: int(C.EINVAL)}
		}
		rid = routerRID.toC()
		ridPointer = &rid
	}
	if err := submitErrorFromResult(C.zlink_select_routed_submit_target(core.raw(), ridPointer, &target)); err != nil {
		return target, err
	}
	return target, nil
}

// submitExactRoutedRequest hands the record to Core with blocking flags. The
// reply handle is armed on the final part only, and is released here when the
// submit fails — otherwise the reply trampoline owns it.
func submitExactRoutedRequest(
	handle unsafe.Pointer,
	role routedRole,
	routerRID *RoutingID,
	target *C.zlink_routed_submit_target_t,
	parts []requestBuilderPart,
	timeoutMillis uint32,
	onReply func(requestResult),
) error {
	state := newReplyCallbackState()
	state.setCompletionDispatch(onReply)
	callbackHandle := cgo.NewHandle(state)
	accepted := false
	defer func() {
		if !accepted {
			callbackHandle.Delete()
		}
	}()

	sendParts := sendPartsFromRequestParts(parts)
	var rid C.zlink_routing_id_t
	if role == routedRouter {
		rid = routerRID.toC()
	}
	err := submitMultipartFromBuilderParts(sendParts, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
		var userdata C.uintptr_t
		var partTimeout uint32
		if partFlag == C.ZLINK_PART_FINAL {
			userdata = C.uintptr_t(callbackHandle)
			partTimeout = timeoutMillis
		}
		if role == routedDealer {
			return submitErrorFromResult(C.zlink_dealer_request_transport_pair_part_go_local(
				handle, target, part, C.ZLINK_SEND_FLAGS_NONE, partFlag,
				C.uint32_t(partTimeout), userdata))
		}
		return submitErrorFromResult(C.zlink_router_request_transport_pair_part_go_local(
			handle, &rid,
			target.transport_pair_id, target.transport_pair_generation,
			part, C.ZLINK_SEND_FLAGS_NONE, partFlag,
			C.uint32_t(partTimeout), userdata))
	})
	accepted = err == nil
	return err
}

// sendPartsFromRequestParts reuses the send builder's ownership rules for the
// request payload: every part is copied for the native submit, and the
// caller's messages are consumed only when the whole record is accepted.
func sendPartsFromRequestParts(parts []requestBuilderPart) []sendBuilderPart {
	converted := make([]sendBuilderPart, len(parts))
	for i, part := range parts {
		converted[i] = sendBuilderPart{
			message: part.message,
			data:    part.data,
			bytes:   part.bytes,
		}
	}
	return converted
}

// requestTimeoutMillis resolves the timeout Core will own. The builder's
// timeout wins when set, otherwise the socket's own request-timeout option is
// used. ctx is deliberately not folded in here: Core owns the request timeout
// (ZLINK_REQUEST_TIMED_OUT) and ctx owns the caller-side completion, so the two
// stay separable and neither has to guess about the other.
func requestTimeoutMillis(core *socketCore, role routedRole, timeout time.Duration) (uint32, error) {
	if timeout <= 0 {
		var err error
		timeout, err = defaultRequestTimeout(core, role)
		if err != nil {
			return 0, err
		}
	}
	return remainingTimeoutMillis(time.Now().Add(timeout)), nil
}

func defaultRequestTimeout(core *socketCore, role routedRole) (time.Duration, error) {
	if core == nil || core.isClosed() {
		return 0, routedTerminalError(int(C.ETERM))
	}
	var raw C.int
	size := C.size_t(C.sizeof_int)
	var err error
	if role == routedDealer {
		err = configErrorFromResult(C.zlink_get_dealer_option(
			core.raw(), C.ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS, unsafe.Pointer(&raw), &size))
	} else {
		err = configErrorFromResult(C.zlink_get_router_option(
			core.raw(), C.ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS, unsafe.Pointer(&raw), &size))
	}
	if err != nil {
		return 0, err
	}
	return time.Duration(raw) * time.Millisecond, nil
}

func remainingTimeoutMillis(deadline time.Time) uint32 {
	remaining := time.Until(deadline)
	if remaining <= 0 {
		return 1
	}
	millis := remaining / time.Millisecond
	if remaining%time.Millisecond != 0 {
		millis++
	}
	if millis > time.Duration(^uint32(0)) {
		return ^uint32(0)
	}
	if millis == 0 {
		return 1
	}
	return uint32(millis)
}

func routedTerminalError(nativeErrno int) error {
	if nativeErrno == 0 {
		nativeErrno = int(C.ENOTCONN)
	}
	result := SubmitNotConnected
	switch nativeErrno {
	case int(C.ETERM), int(C.ESHUTDOWN), int(C.ECANCELED):
		result = SubmitTerminated
	case int(C.ENOENT), int(C.EHOSTUNREACH):
		result = SubmitNotFound
	}
	return &SubmitError{Result: result, nativeErrno: nativeErrno}
}

func requestCompletionFromError(err error) RequestReplyCompletion {
	completion := RequestReplyCompletion{Err: err}
	if err == nil {
		return completion
	}
	var requestError *RequestError
	if errors.As(err, &requestError) {
		completion.Result = requestError.Result
		return completion
	}
	if errors.Is(err, context.DeadlineExceeded) {
		completion.Result = RequestTimedOut
		return completion
	}
	if errors.Is(err, context.Canceled) {
		completion.Result = RequestTerminated
		return completion
	}
	var submitError *SubmitError
	if errors.As(err, &submitError) {
		switch submitError.Result {
		case SubmitBackpressured:
			if errors.Is(err, syscall.ETIMEDOUT) {
				completion.Result = RequestTimedOut
			} else {
				completion.Result = RequestBackpressured
			}
		case SubmitNotConnected:
			completion.Result = RequestNotConnected
		case SubmitNotFound:
			completion.Result = RequestNotFound
		case SubmitTerminated:
			completion.Result = RequestTerminated
		case SubmitInvalidArgument:
			completion.Result = RequestInvalidArgument
		case SubmitNotSupported:
			completion.Result = RequestNotSupported
		default:
			completion.Result = RequestInternalError
		}
		return completion
	}
	completion.Result = RequestInternalError
	return completion
}
