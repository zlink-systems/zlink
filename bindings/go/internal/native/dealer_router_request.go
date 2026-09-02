// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <errno.h>
#include <stdint.h>
#include "zlink.h"
*/
import "C"

import (
	"context"
	"time"
	"unsafe"
)

func submitCompletionSend(
	ctx context.Context,
	core *socketCore,
	target *RoutingID,
	stream bool,
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

	entry := newCompletionEntry(completionSend, ctx)
	if err := core.completion.register(entry); err != nil {
		entry.failSubmit()
		entry.deleteHandle()
		return err
	}
	if err := contextError(ctx); err != nil {
		entry.failSubmit()
		core.completion.unregister(entry)
		return err
	}

	var completionID C.zlink_completion_id_t
	var rid C.zlink_routing_id_t
	if target != nil {
		rid = target.toC()
	}
	submitPart := func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
		var userContext unsafe.Pointer
		var completionOut *C.zlink_completion_id_t
		if partFlag == C.ZLINK_PART_FINAL {
			userContext = entry.userContext()
			completionOut = &completionID
		}
		if target == nil {
			return submitErrorFromResult(C.zlink_send_part(
				core.raw(), part, C.ZLINK_SEND_FLAGS_DONTWAIT, partFlag,
				userContext, completionOut))
		}
		return submitErrorFromResult(C.zlink_send_part_rid(
			core.raw(), &rid, part, C.ZLINK_SEND_FLAGS_DONTWAIT, partFlag,
			userContext, completionOut))
	}

	var err error
	if stream {
		err = submitStreamFromBuilderParts(parts, submitPart)
	} else {
		err = submitMultipartFromBuilderParts(parts, submitPart)
	}
	if err != nil {
		entry.failSubmit()
		core.completion.unregister(entry)
		return err
	}

	entry.publish(uint64(completionID))
	if completionID == 0 {
		entry.capture(nil, nil)
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
		var userContext unsafe.Pointer
		var completionOut *C.zlink_completion_id_t
		if partFlag == C.ZLINK_PART_FINAL {
			partTimeout = C.uint32_t(timeoutMillis)
			userContext = entry.userContext()
			completionOut = &completionID
		}
		return submitErrorFromResult(C.zlink_request_part(
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
