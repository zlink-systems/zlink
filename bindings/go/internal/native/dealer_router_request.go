// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <errno.h>
#include <stdint.h>
#include "zlink.h"

static inline zlink_submit_result_t zlink_go_send_part_with_context(
    void *socket_, zlink_msg_t *part_, zlink_send_flags_t flags_,
    zlink_part_flag_t part_flag_, uintptr_t user_context_,
    zlink_completion_id_t *completion_id_out_) {
    return zlink_send_part(socket_, part_, flags_, part_flag_,
                           (void *)user_context_, completion_id_out_);
}

static inline zlink_submit_result_t zlink_go_send_part_rid_with_context(
    void *socket_, const zlink_routing_id_t *target_rid_, zlink_msg_t *part_,
    zlink_send_flags_t flags_, zlink_part_flag_t part_flag_,
    uintptr_t user_context_, zlink_completion_id_t *completion_id_out_) {
    return zlink_send_part_rid(socket_, target_rid_, part_, flags_, part_flag_,
                               (void *)user_context_, completion_id_out_);
}

static inline zlink_submit_result_t zlink_go_request_part_with_context(
    void *socket_, const zlink_routing_id_t *target_rid_, zlink_msg_t *part_,
    zlink_send_flags_t flags_, zlink_part_flag_t part_flag_,
    uint32_t timeout_ms_, uintptr_t user_context_,
    zlink_completion_id_t *completion_id_out_) {
    return zlink_request_part(socket_, target_rid_, part_, flags_, part_flag_,
                              timeout_ms_, (void *)user_context_,
                              completion_id_out_);
}
*/
import "C"

import (
	"context"
	"errors"
	"time"
)

type sendRetryState struct {
	core      *socketCore
	target    RoutingID
	hasTarget bool
	payload   *sendRetryPayload
}

func newSendRetryState(
	core *socketCore,
	target *RoutingID,
	parts []sendBuilderPart,
) (*sendRetryState, error) {
	payload, err := newSendRetryPayload(parts)
	if err != nil {
		return nil, err
	}
	state := &sendRetryState{core: core, payload: payload}
	if target != nil {
		state.target = *target
		state.hasTarget = true
	}
	return state, nil
}

func (s *sendRetryState) attempt(userContext uintptr) (uint64, error) {
	if s == nil || s.core == nil || s.payload == nil {
		return 0, &SubmitError{Result: SubmitInvalidHandle, nativeErrno: int(C.EFAULT)}
	}

	var completionID C.zlink_completion_id_t
	var rid C.zlink_routing_id_t
	if s.hasTarget {
		rid = s.target.toC()
	}
	err := submitMultipartFromClones(s.payload.owned, false, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
		var finalContext C.uintptr_t
		var completionOut *C.zlink_completion_id_t
		if partFlag == C.ZLINK_PART_FINAL {
			finalContext = C.uintptr_t(userContext)
			completionOut = &completionID
		}
		if !s.hasTarget {
			return submitErrorFromResult(C.zlink_go_send_part_with_context(
				s.core.raw(), part, C.ZLINK_SEND_FLAGS_DONTWAIT, partFlag,
				finalContext, completionOut))
		}
		return submitErrorFromResult(C.zlink_go_send_part_rid_with_context(
			s.core.raw(), &rid, part, C.ZLINK_SEND_FLAGS_DONTWAIT, partFlag,
			finalContext, completionOut))
	})
	return uint64(completionID), err
}

func (s *sendRetryState) matchesTarget(actual RoutingID) bool {
	if s == nil {
		return false
	}
	if !s.hasTarget {
		return actual.Size() == 0
	}
	return s.target.Equal(actual)
}

func (e *completionEntry) attemptSend() bool {
	if e == nil {
		return true
	}
	e.attemptMu.Lock()
	defer e.attemptMu.Unlock()

	e.mu.Lock()
	settled := e.settled
	e.mu.Unlock()
	if settled {
		return true
	}
	if e.send == nil {
		e.finishSend(&SubmitError{Result: SubmitInternalError, nativeErrno: int(C.EPROTO)})
		return true
	}

	completionID, err := e.send.attempt(e.handleKey)
	if err == nil {
		if completionID != 0 {
			err = &SubmitError{Result: SubmitInternalError, nativeErrno: int(C.EPROTO)}
		} else {
			e.setSendWaiting(false)
			e.send.payload.takeSourceOwnership()
			e.send.payload.close()
			e.finishSend(nil)
			return true
		}
	}

	var submitErr *SubmitError
	if errors.As(err, &submitErr) && submitErr.Result == SubmitBackpressured {
		if submitErr.internalErrno() == int(C.EAGAIN) && completionID != 0 {
			e.send.payload.takeSourceOwnership()
			e.publishSendWait(completionID)
			if activateErr := e.setSendWaiting(true); activateErr != nil {
				// Core already owns the token. Preserve the entry as a tombstone so
				// its cgo context cannot be reused before socket shutdown.
				e.mu.Lock()
				if !e.publicDone {
					e.err = activateErr
					e.publicDone = true
					close(e.done)
				}
				e.mu.Unlock()
				e.send.payload.close()
			}
			return false
		}
		err = &SubmitError{Result: SubmitInternalError, nativeErrno: int(C.EPROTO)}
	}

	e.setSendWaiting(false)
	e.send.payload.close()
	e.finishSend(err)
	return true
}

func submitManagedSend(
	ctx context.Context,
	core *socketCore,
	target *RoutingID,
	parts []sendBuilderPart,
) error {
	if err := contextError(ctx); err != nil {
		return err
	}
	if core == nil || core.isClosed() {
		return &SubmitError{Result: SubmitInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	if target != nil && target.Size() == 0 {
		return &SubmitError{Result: SubmitInvalidArgument, nativeErrno: int(C.EINVAL)}
	}

	send, err := newSendRetryState(core, target, parts)
	if err != nil {
		return err
	}
	entry := newSendCompletionEntry(ctx, send)
	if err := core.completion.register(entry); err != nil {
		send.payload.close()
		entry.failSubmit()
		entry.deleteHandle()
		return err
	}
	if err := contextError(ctx); err != nil {
		send.payload.close()
		entry.finishSend(err)
		core.completion.unregister(entry)
		return err
	}

	if entry.attemptSend() {
		core.completion.unregister(entry)
	}
	return entry.waitSend()
}

func submitCompletionRequest(
	ctx context.Context,
	core *socketCore,
	target *RoutingID,
	timeout time.Duration,
	parts []requestBuilderPart,
) ([]*Message, error) {
	if err := contextError(ctx); err != nil {
		return nil, err
	}
	if core == nil || core.isClosed() {
		return nil, &SubmitError{Result: SubmitInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	if target != nil && target.Size() == 0 {
		return nil, &SubmitError{Result: SubmitInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	timeoutMillis, err := requestTimeoutValue(timeout)
	if err != nil {
		return nil, err
	}

	entry := newCompletionEntry(completionRequest, ctx)
	if err := core.completion.register(entry); err != nil {
		entry.failSubmit()
		entry.deleteHandle()
		return nil, err
	}
	if err := contextError(ctx); err != nil {
		entry.failSubmit()
		core.completion.unregister(entry)
		return nil, err
	}

	var completionID C.zlink_completion_id_t
	var rid C.zlink_routing_id_t
	var ridPointer *C.zlink_routing_id_t
	if target != nil {
		rid = target.toC()
		ridPointer = &rid
	}
	sendParts := sendPartsFromRequestParts(parts)
	err = submitMultipartFromBuilderParts(sendParts, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
		var partTimeout C.uint32_t
		var userContext C.uintptr_t
		var completionOut *C.zlink_completion_id_t
		if partFlag == C.ZLINK_PART_FINAL {
			partTimeout = C.uint32_t(timeoutMillis)
			userContext = C.uintptr_t(entry.handleKey)
			completionOut = &completionID
		}
		return submitErrorFromResult(C.zlink_go_request_part_with_context(
			core.raw(), ridPointer, part, C.ZLINK_SEND_FLAGS_DONTWAIT, partFlag,
			partTimeout, userContext, completionOut))
	})
	if err != nil {
		entry.failSubmit()
		core.completion.unregister(entry)
		return nil, err
	}
	if completionID == 0 {
		entry.failSubmit()
		core.completion.unregister(entry)
		return nil, &SubmitError{Result: SubmitInternalError, nativeErrno: int(C.EPROTO)}
	}

	entry.publish(uint64(completionID))
	return entry.waitRequest()
}

func sendPartsFromRequestParts(parts []requestBuilderPart) []sendBuilderPart {
	converted := make([]sendBuilderPart, len(parts))
	for i, part := range parts {
		converted[i] = sendBuilderPart{message: part.message, data: part.data, bytes: part.bytes}
	}
	return converted
}

func requestTimeoutValue(timeout time.Duration) (uint32, error) {
	if timeout < 0 {
		return 0, &SubmitError{Result: SubmitInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	if timeout == 0 {
		return 0, nil
	}
	millis := timeout / time.Millisecond
	if timeout%time.Millisecond != 0 {
		millis++
	}
	if millis > time.Duration(^uint32(0)) {
		return ^uint32(0), nil
	}
	if millis == 0 {
		return 1, nil
	}
	return uint32(millis), nil
}
