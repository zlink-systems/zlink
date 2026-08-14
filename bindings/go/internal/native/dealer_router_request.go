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

type routedPayload struct {
	once              sync.Once
	owned             []*Message
	consumeOnAccepted []*Message
}

func snapshotSendPayload(parts []sendBuilderPart) (*routedPayload, error) {
	if len(parts) == 0 {
		return nil, configInvalidArgumentError()
	}
	payload := &routedPayload{}
	moveSources := make([]*Message, 0, len(parts))
	for _, part := range parts {
		var owned *Message
		var err error
		switch {
		case part.bytes:
			owned, err = NewMessage(part.data)
		case part.message == nil || part.message.closed:
			err = configInvalidArgumentError()
		default:
			owned, err = part.message.clone()
			if err == nil {
				if part.move {
					moveSources = append(moveSources, part.message)
				} else {
					payload.consumeOnAccepted = append(payload.consumeOnAccepted, part.message)
				}
			}
		}
		if err != nil {
			payload.resolve(false)
			return nil, err
		}
		payload.owned = append(payload.owned, owned)
	}
	for _, source := range moveSources {
		_ = source.Close()
	}
	return payload, nil
}

func snapshotRequestPayload(parts []requestBuilderPart) (*routedPayload, error) {
	if len(parts) == 0 {
		return nil, configInvalidArgumentError()
	}
	payload := &routedPayload{}
	for _, part := range parts {
		var owned *Message
		var err error
		if part.bytes {
			owned, err = NewMessage(part.data)
		} else if part.message == nil || part.message.closed {
			err = configInvalidArgumentError()
		} else {
			owned, err = part.message.clone()
			if err == nil {
				payload.consumeOnAccepted = append(payload.consumeOnAccepted, part.message)
			}
		}
		if err != nil {
			payload.resolve(false)
			return nil, err
		}
		payload.owned = append(payload.owned, owned)
	}
	return payload, nil
}

func (p *routedPayload) resolve(accepted bool) {
	if p == nil {
		return
	}
	p.once.Do(func() {
		closeMessageSlice(p.owned)
		p.owned = nil
		if accepted {
			closeMessageSlice(p.consumeOnAccepted)
		}
		p.consumeOnAccepted = nil
	})
}

type routedSendCompletion struct {
	mu       sync.Mutex
	done     bool
	result   chan error
	payload  *routedPayload
	stopCtx  func() bool
	deadline *time.Timer
}

func (s *routedSendCompletion) install(stopCtx func() bool, deadline *time.Timer) {
	s.mu.Lock()
	if s.done {
		s.mu.Unlock()
		if stopCtx != nil {
			stopCtx()
		}
		if deadline != nil {
			deadline.Stop()
		}
		return
	}
	s.stopCtx = stopCtx
	s.deadline = deadline
	s.mu.Unlock()
}

func (s *routedSendCompletion) complete(err error, accepted bool) {
	s.mu.Lock()
	if s.done {
		s.mu.Unlock()
		if accepted {
			s.payload.resolve(true)
		}
		return
	}
	s.done = true
	stopCtx := s.stopCtx
	deadline := s.deadline
	s.stopCtx = nil
	s.deadline = nil
	s.mu.Unlock()

	s.payload.resolve(accepted)
	if stopCtx != nil {
		stopCtx()
	}
	if deadline != nil {
		deadline.Stop()
	}
	s.result <- err
	close(s.result)
}

type routedRequestCompletion struct {
	mu           sync.Mutex
	accepted     bool
	done         bool
	pendingReply *RequestReplyCompletion
	pendingError error
	result       chan RequestReplyCompletion
	payload      *routedPayload
	stopCtx      func() bool
	deadline     *time.Timer
}

func (r *routedRequestCompletion) install(stopCtx func() bool, deadline *time.Timer) {
	r.mu.Lock()
	if r.done {
		r.mu.Unlock()
		if stopCtx != nil {
			stopCtx()
		}
		if deadline != nil {
			deadline.Stop()
		}
		return
	}
	r.stopCtx = stopCtx
	r.deadline = deadline
	r.mu.Unlock()
}

func (r *routedRequestCompletion) onAccepted() {
	r.payload.resolve(true)
	var completion *RequestReplyCompletion
	var terminalError error
	var stopCtx func() bool
	var deadline *time.Timer
	r.mu.Lock()
	r.accepted = true
	if !r.done && (r.pendingError != nil || r.pendingReply != nil) {
		terminalError = r.pendingError
		if terminalError != nil {
			value := requestCompletionFromError(terminalError)
			completion = &value
		} else {
			completion = r.pendingReply
		}
		r.pendingError = nil
		pendingReply := r.pendingReply
		r.pendingReply = nil
		r.done = true
		stopCtx = r.stopCtx
		deadline = r.deadline
		r.stopCtx = nil
		r.deadline = nil
		if terminalError != nil && pendingReply != nil {
			MultipartClose(pendingReply.Parts)
		}
	}
	r.mu.Unlock()
	if completion != nil {
		r.deliver(*completion, stopCtx, deadline)
	}
}

func (r *routedRequestCompletion) cancelAfterAdmission(err error) {
	var completion *RequestReplyCompletion
	var pending *RequestReplyCompletion
	var stopCtx func() bool
	var deadline *time.Timer
	r.mu.Lock()
	if r.done {
		r.mu.Unlock()
		return
	}
	if !r.accepted {
		if r.pendingError == nil {
			r.pendingError = err
		}
		r.mu.Unlock()
		return
	}
	value := requestCompletionFromError(err)
	completion = &value
	pending = r.pendingReply
	r.pendingReply = nil
	r.pendingError = nil
	r.done = true
	stopCtx = r.stopCtx
	deadline = r.deadline
	r.stopCtx = nil
	r.deadline = nil
	r.mu.Unlock()
	if pending != nil {
		MultipartClose(pending.Parts)
	}
	r.deliver(*completion, stopCtx, deadline)
}

func (r *routedRequestCompletion) onReply(result requestResult) {
	completion := RequestReplyCompletion{
		Result: result.result,
		Parts:  result.parts,
		Err:    requestCompletionError(result.result),
	}
	var deliver bool
	var stopCtx func() bool
	var deadline *time.Timer
	r.mu.Lock()
	if r.done {
		r.mu.Unlock()
		MultipartClose(completion.Parts)
		return
	}
	if !r.accepted {
		r.pendingReply = &completion
		r.mu.Unlock()
		return
	}
	r.done = true
	deliver = true
	stopCtx = r.stopCtx
	deadline = r.deadline
	r.stopCtx = nil
	r.deadline = nil
	r.mu.Unlock()
	if deliver {
		r.deliver(completion, stopCtx, deadline)
	}
}

func (r *routedRequestCompletion) fail(err error) {
	r.payload.resolve(false)
	completion := requestCompletionFromError(err)
	var pending *RequestReplyCompletion
	var stopCtx func() bool
	var deadline *time.Timer
	r.mu.Lock()
	if r.done {
		r.mu.Unlock()
		return
	}
	r.done = true
	pending = r.pendingReply
	r.pendingReply = nil
	r.pendingError = nil
	stopCtx = r.stopCtx
	deadline = r.deadline
	r.stopCtx = nil
	r.deadline = nil
	r.mu.Unlock()
	if pending != nil {
		MultipartClose(pending.Parts)
	}
	r.deliver(completion, stopCtx, deadline)
}

func (r *routedRequestCompletion) deliver(completion RequestReplyCompletion, stopCtx func() bool, deadline *time.Timer) {
	if stopCtx != nil {
		stopCtx()
	}
	if deadline != nil {
		deadline.Stop()
	}
	r.result <- completion
	close(r.result)
}

func submitRoutedSend(ctx context.Context, admission *routedAdmission, routerRID *RoutingID, parts []sendBuilderPart) <-chan error {
	started := time.Now()
	result := make(chan error, 1)
	if err := contextError(ctx); err != nil {
		result <- err
		close(result)
		return result
	}
	payload, err := snapshotSendPayload(parts)
	if err != nil {
		result <- err
		close(result)
		return result
	}
	deadline, deadlineError, attemptBeforeExpiry, err := routedSendDeadline(admission, ctx, started)
	if err != nil {
		payload.resolve(false)
		result <- err
		close(result)
		return result
	}
	completion := &routedSendCompletion{result: result, payload: payload}
	operation := &routedPendingOperation{
		context:             ctx,
		deadline:            deadline,
		deadlineError:       deadlineError,
		attemptBeforeExpiry: attemptBeforeExpiry,
		accepted:            func() { completion.complete(nil, true) },
		terminal:            func(err error) { completion.complete(err, false) },
	}
	operation.attempt = func(handle unsafe.Pointer, target *C.zlink_routed_submit_target_t) error {
		return attemptExactRoutedSend(handle, admission.role, routerRID, target, payload.owned)
	}
	effectiveAttemptBeforeExpiry, err := admission.enqueue(routerRID, operation)
	if err != nil {
		completion.complete(err, false)
		return result
	}
	stopCtx, deadlineTimer := watchRoutedOperation(
		ctx, deadline, effectiveAttemptBeforeExpiry,
		func(err error) {
			admission.cancel(operation.id, err)
		},
		deadlineError,
	)
	completion.install(stopCtx, deadlineTimer)
	admission.activate(operation.id)
	return result
}

func submitRoutedRequest(ctx context.Context, admission *routedAdmission, routerRID *RoutingID, timeout time.Duration, parts []requestBuilderPart) <-chan RequestReplyCompletion {
	started := time.Now()
	result := make(chan RequestReplyCompletion, 1)
	if err := contextError(ctx); err != nil {
		result <- requestCompletionFromError(err)
		close(result)
		return result
	}
	payload, err := snapshotRequestPayload(parts)
	if err != nil {
		result <- requestCompletionFromError(err)
		close(result)
		return result
	}
	deadline, deadlineError, err := routedRequestDeadline(admission, ctx, timeout, started)
	if err != nil {
		payload.resolve(false)
		result <- requestCompletionFromError(err)
		close(result)
		return result
	}
	completion := &routedRequestCompletion{result: result, payload: payload}
	operation := &routedPendingOperation{
		context:       ctx,
		deadline:      deadline,
		deadlineError: deadlineError,
		accepted:      completion.onAccepted,
		terminal:      completion.fail,
	}
	operation.attempt = func(handle unsafe.Pointer, target *C.zlink_routed_submit_target_t) error {
		return attemptExactRoutedRequest(
			handle, admission.role, routerRID, target, payload.owned,
			deadline, completion.onReply)
	}
	_, err = admission.enqueue(routerRID, operation)
	if err != nil {
		completion.fail(err)
		return result
	}
	stopCtx, deadlineTimer := watchRoutedOperation(
		ctx, deadline, false,
		func(err error) {
			if !admission.cancel(operation.id, err) {
				completion.cancelAfterAdmission(err)
			}
		},
		deadlineError,
	)
	completion.install(stopCtx, deadlineTimer)
	admission.activate(operation.id)
	return result
}

func attemptExactRoutedSend(
	handle unsafe.Pointer,
	role routedAdmissionRole,
	routerRID *RoutingID,
	target *C.zlink_routed_submit_target_t,
	parts []*Message,
) error {
	return submitMultipartFromClones(parts, false, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
		if role == routedDealer {
			return submitErrorFromResult(C.zlink_dealer_send_transport_pair_part(
				handle, target, part, C.ZLINK_SEND_FLAGS_DONTWAIT, partFlag))
		}
		if routerRID == nil {
			return &SubmitError{Result: SubmitInvalidArgument, nativeErrno: int(C.EINVAL)}
		}
		rid := routerRID.toC()
		return submitErrorFromResult(C.zlink_send_part_transport_pair(
			handle, &rid,
			target.transport_pair_id, target.transport_pair_generation,
			part, C.ZLINK_SEND_FLAGS_DONTWAIT, partFlag))
	})
}

func attemptExactRoutedRequest(
	handle unsafe.Pointer,
	role routedAdmissionRole,
	routerRID *RoutingID,
	target *C.zlink_routed_submit_target_t,
	parts []*Message,
	deadline time.Time,
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

	err := submitMultipartFromClones(parts, false, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
		final := partFlag == C.ZLINK_PART_FINAL
		var userdata C.uintptr_t
		var timeoutMillis uint32
		if final {
			userdata = C.uintptr_t(callbackHandle)
			timeoutMillis = remainingTimeoutMillis(deadline)
		}
		if role == routedDealer {
			return submitErrorFromResult(C.zlink_dealer_request_transport_pair_part_go_local(
				handle, target, part, C.ZLINK_SEND_FLAGS_DONTWAIT, partFlag,
				C.uint32_t(timeoutMillis), userdata))
		}
		if routerRID == nil {
			return &SubmitError{Result: SubmitInvalidArgument, nativeErrno: int(C.EINVAL)}
		}
		rid := routerRID.toC()
		return submitErrorFromResult(C.zlink_router_request_transport_pair_part_go_local(
			handle, &rid,
			target.transport_pair_id, target.transport_pair_generation,
			part, C.ZLINK_SEND_FLAGS_DONTWAIT, partFlag,
			C.uint32_t(timeoutMillis), userdata))
	})
	accepted = err == nil
	return err
}

func routedSendDeadline(admission *routedAdmission, ctx context.Context, started time.Time) (time.Time, error, bool, error) {
	timeout, err := routedCommonTimeout(admission, C.ZLINK_OPT_SNDTIMEO)
	if err != nil {
		return time.Time{}, nil, false, err
	}
	var deadline time.Time
	deadlineError := error(&SubmitError{Result: SubmitBackpressured, nativeErrno: int(C.ETIMEDOUT)})
	attemptBeforeExpiry := false
	if timeout >= 0 {
		deadline = started.Add(timeout)
		attemptBeforeExpiry = timeout == 0
	}
	if contextDeadline, ok := contextDeadline(ctx); ok && (deadline.IsZero() || !contextDeadline.After(deadline)) {
		deadline = contextDeadline
		deadlineError = context.DeadlineExceeded
		attemptBeforeExpiry = false
	}
	return deadline, deadlineError, attemptBeforeExpiry, nil
}

func routedRequestDeadline(admission *routedAdmission, ctx context.Context, timeout time.Duration, started time.Time) (time.Time, error, error) {
	if timeout <= 0 {
		var err error
		timeout, err = routedDefaultRequestTimeout(admission)
		if err != nil {
			return time.Time{}, nil, err
		}
	}
	deadline := started.Add(timeout)
	deadlineError := error(&RequestError{Result: RequestTimedOut, nativeErrno: int(C.ETIMEDOUT)})
	if contextDeadline, ok := contextDeadline(ctx); ok && !contextDeadline.After(deadline) {
		deadline = contextDeadline
		deadlineError = context.DeadlineExceeded
	}
	return deadline, deadlineError, nil
}

func routedCommonTimeout(admission *routedAdmission, option C.zlink_option_t) (time.Duration, error) {
	if admission == nil {
		return 0, routedTerminalError(int(C.ETERM))
	}
	admission.attemptGate.Lock()
	defer admission.attemptGate.Unlock()
	admission.mu.Lock()
	unavailable := admission.closed || admission.closing
	admission.mu.Unlock()
	if unavailable || admission.core == nil || admission.core.isClosed() {
		return 0, routedTerminalError(int(C.ETERM))
	}
	return admission.core.getDurationOption(option)
}

func routedDefaultRequestTimeout(admission *routedAdmission) (time.Duration, error) {
	if admission == nil {
		return 0, routedTerminalError(int(C.ETERM))
	}
	admission.attemptGate.Lock()
	defer admission.attemptGate.Unlock()
	admission.mu.Lock()
	unavailable := admission.closed || admission.closing
	admission.mu.Unlock()
	if unavailable || admission.core == nil || admission.core.isClosed() {
		return 0, routedTerminalError(int(C.ETERM))
	}
	var raw C.int
	size := C.size_t(C.sizeof_int)
	var err error
	if admission.role == routedDealer {
		err = configErrorFromResult(C.zlink_get_dealer_option(
			admission.core.raw(), C.ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS,
			unsafe.Pointer(&raw), &size))
	} else {
		err = configErrorFromResult(C.zlink_get_router_option(
			admission.core.raw(), C.ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS,
			unsafe.Pointer(&raw), &size))
	}
	if err != nil {
		return 0, err
	}
	return time.Duration(raw) * time.Millisecond, nil
}

func watchRoutedOperation(
	ctx context.Context,
	deadline time.Time,
	attemptBeforeExpiry bool,
	cancel func(error),
	deadlineError error,
) (func() bool, *time.Timer) {
	var stopCtx func() bool
	if ctx != nil && ctx.Done() != nil {
		stopCtx = context.AfterFunc(ctx, func() {
			cancel(ctx.Err())
		})
	}
	var deadlineTimer *time.Timer
	if !deadline.IsZero() && !attemptBeforeExpiry {
		deadlineTimer = time.AfterFunc(time.Until(deadline), func() {
			cancel(deadlineError)
		})
	}
	return stopCtx, deadlineTimer
}

func contextDeadline(ctx context.Context) (time.Time, bool) {
	if ctx == nil {
		return time.Time{}, false
	}
	return ctx.Deadline()
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
