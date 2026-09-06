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

// requestRetryState owns the logical request only after Core refuses the
// initial admission attempt and returns a WRITABLE wait token. Once admitted,
// Core owns the request lifecycle and the entry waits for its normal REQUEST
// completion (reply or timeout).
type requestRetryState struct {
	core      *socketCore
	target    RoutingID
	hasTarget bool
	timeout   uint32
	payload   *sendRetryPayload
}

func newRequestRetryState(
	core *socketCore,
	target *RoutingID,
	timeout uint32,
	parts []requestBuilderPart,
) (*requestRetryState, error) {
	state := &requestRetryState{core: core, timeout: timeout}
	if target != nil {
		state.target = *target
		state.hasTarget = true
	}
	payload, err := newSendRetryPayload(parts)
	state.payload = payload
	return state, err
}

func newSendRetryState(
	core *socketCore,
	target *RoutingID,
	parts []sendBuilderPart,
) (sendRetryState, error) {
	payload, err := newSendRetryPayload(parts)
	if err != nil {
		return sendRetryState{}, err
	}
	state := sendRetryState{core: core, payload: payload}
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
	var rid *C.zlink_routing_id_t
	if s.hasTarget {
		value := s.target.toC()
		rid = &value
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
			s.core.raw(), rid, part, C.ZLINK_SEND_FLAGS_DONTWAIT, partFlag,
			finalContext, completionOut))
	})
	return uint64(completionID), err
}

func (s *requestRetryState) attempt(userContext uintptr) (uint64, error) {
	if s == nil || s.core == nil || s.payload == nil {
		return 0, &SubmitError{Result: SubmitInvalidHandle, nativeErrno: int(C.EFAULT)}
	}

	var completionID C.zlink_completion_id_t
	var ridPointer *C.zlink_routing_id_t
	if s.hasTarget {
		rid := s.target.toC()
		ridPointer = &rid
	}
	err := submitMultipartFromClones(s.payload.owned, false, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
		var partTimeout C.uint32_t
		var finalContext C.uintptr_t
		var completionOut *C.zlink_completion_id_t
		if partFlag == C.ZLINK_PART_FINAL {
			partTimeout = C.uint32_t(s.timeout)
			finalContext = C.uintptr_t(userContext)
			completionOut = &completionID
		}
		return submitErrorFromResult(C.zlink_go_request_part_with_context(
			s.core.raw(), ridPointer, part, C.ZLINK_SEND_FLAGS_DONTWAIT, partFlag,
			partTimeout, finalContext, completionOut))
	})
	return uint64(completionID), err
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
			e.setWritableWaiting(false)
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
			if activateErr := e.setWritableWaiting(true); activateErr != nil {
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

	e.setWritableWaiting(false)
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
	if err := contextError(ctx); err != nil {
		send.payload.close()
		return err
	}
	// Only the nonblocking native admission and wait-token publication share
	// the drain owner's lock. No completion wait or payload preparation holds it.
	key := nextCompletionContext()
	owner := core.completion
	owner.mu.Lock()
	if owner.shutdown {
		owner.mu.Unlock()
		send.payload.close()
		return &SubmitError{Result: SubmitInvalidState, nativeErrno: int(C.ESHUTDOWN)}
	}
	completionID, err := send.attempt(key)
	if err == nil && completionID == 0 {
		owner.mu.Unlock()
		send.payload.takeSourceOwnership()
		send.payload.close()
		return nil
	}
	var submitErr *SubmitError
	if !errors.As(err, &submitErr) || submitErr.Result != SubmitBackpressured ||
		submitErr.internalErrno() != int(C.EAGAIN) || completionID == 0 {
		owner.mu.Unlock()
		send.payload.close()
		if err == nil || (submitErr != nil && submitErr.Result == SubmitBackpressured) {
			return &SubmitError{Result: SubmitInternalError, nativeErrno: int(C.EPROTO)}
		}
		return err
	}
	// A token now exists. Publish the entry before a drain can look it up;
	// immediate admission has no entry, channel, or global handle registration.
	retry := new(sendRetryState)
	*retry = send
	entry := newSendCompletionEntry(nil, retry, key)
	entry.owner = owner
	entry.attemptMu.Lock()
	owner.entries[key] = entry
	owner.mu.Unlock()
	send.payload.takeSourceOwnership()
	entry.publishSendWait(completionID)
	if activateErr := entry.setWritableWaiting(true); activateErr != nil {
		entry.mu.Lock()
		entry.err = activateErr
		entry.publicDone = true
		close(entry.done)
		entry.mu.Unlock()
		send.payload.close()
	}
	entry.attemptMu.Unlock()
	entry.enableCancellation(ctx)
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
		return nil, err
	}
	if err := contextError(ctx); err != nil {
		entry.failSubmit()
		core.completion.unregister(entry)
		return nil, err
	}

	var completionID C.zlink_completion_id_t
	var ridPointer *C.zlink_routing_id_t
	if target != nil {
		rid := target.toC()
		ridPointer = &rid
	}
	err = submitMultipartFromBuilderParts(parts, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
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
	if err == nil {
		if completionID == 0 {
			entry.failSubmit()
			core.completion.unregister(entry)
			return nil, &SubmitError{Result: SubmitInternalError, nativeErrno: int(C.EPROTO)}
		}
		entry.publish(uint64(completionID))
		return entry.waitRequest()
	}

	var submitErr *SubmitError
	if errors.As(err, &submitErr) && submitErr.Result == SubmitBackpressured &&
		submitErr.internalErrno() == int(C.EAGAIN) && completionID != 0 {
		entry.attemptMu.Lock()
		retry, snapshotErr := newRequestRetryState(core, target, timeoutMillis, parts)
		entry.request = retry
		earlyWritable := entry.publishRequestWait(uint64(completionID))
		if snapshotErr == nil {
			retry.payload.takeSourceOwnership()
		}
		activateErr := entry.setWritableWaiting(true)
		if snapshotErr != nil || activateErr != nil {
			if snapshotErr == nil {
				snapshotErr = requestWaitActivationError(activateErr)
			}
			entry.mu.Lock()
			if !entry.publicDone {
				entry.err = snapshotErr
				entry.publicDone = true
				close(entry.done)
			}
			entry.mu.Unlock()
			if retry.payload != nil {
				retry.payload.close()
			}
		}
		entry.attemptMu.Unlock()
		if earlyWritable != nil && entry.captureRequestWritableRecord(*earlyWritable) {
			core.completion.unregister(entry)
		}
		return entry.waitRequest()
	}

	if err != nil {
		entry.failSubmit()
		core.completion.unregister(entry)
		return nil, err
	}
	return nil, &SubmitError{Result: SubmitInternalError, nativeErrno: int(C.EPROTO)}
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

func requestWaitActivationError(err error) error {
	var submitErr *SubmitError
	if errors.As(err, &submitErr) && submitErr.Result == SubmitTerminated {
		return requestTerminalError(submitErr.internalErrno())
	}
	return err
}
