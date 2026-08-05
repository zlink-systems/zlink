// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <errno.h>
#include <stdint.h>
#include "zlink.h"

extern void goZlinkCompletionControlTrampoline(zlink_routing_id_t *source_rid_, zlink_msg_t *parts_, size_t part_count_, uintptr_t userdata_);

static inline int zlink_router_completion_control_handler_go_local(void *router, uintptr_t userdata) {
    return zlink_router_completion_control_handler(router, (zlink_completion_control_handler_fn)goZlinkCompletionControlTrampoline, (void *)userdata);
}
*/
import "C"

import "runtime/cgo"

// OnCompletionControl installs the typed callback for opaque ROUTER
// completion-control records. The callback receives a common Received
// envelope; each payload Message belongs to the callback until it is closed or
// submitted through a supported operation.
func (s *RouterSocket) OnCompletionControl(handler func(*Received)) error {
	if handler == nil {
		return &HandlerError{Result: HandlerInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	if s == nil || s.socketCore == nil || s.socketCore.isClosed() {
		return &HandlerError{Result: HandlerInvalidArgument, nativeErrno: int(C.EFAULT)}
	}
	state := newCompletionControlCallbackState(handler)
	handle := cgo.NewHandle(state)
	err := s.socketCore.replaceCallback(handle, &s.completionControlHandle, nil, func() error {
		return handlerErrorFromResult(C.zlink_router_completion_control_handler_go_local(s.raw(), C.uintptr_t(handle)))
	})
	if err != nil {
		state.close()
		handle.Delete()
		return err
	}
	return nil
}

// CompletionControl returns an operation builder for an opaque control record
// addressed to a ROUTER peer. The Core control plane accepts no send flags;
// flags other than SendFlagsNone are rejected at submit time.
func (s *RouterSocket) CompletionControl(peerRID RoutingID) SendOp {
	return newSendBuilder(func(parts []sendBuilderPart, flags SendFlags) error {
		if flags != SendFlagsNone {
			return &SubmitError{Result: SubmitNotSupported, nativeErrno: int(C.ENOTSUP)}
		}
		target := peerRID.toC()
		return submitMultipartFromBuilderParts(parts, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
			return submitErrorFromResult(C.zlink_router_completion_control_part(
				s.raw(),
				&target,
				part,
				partFlag,
			))
		})
	})
}

//export goZlinkCompletionControlTrampoline
func goZlinkCompletionControlTrampoline(sourceRID *C.zlink_routing_id_t, parts *C.zlink_msg_t, partCount C.size_t, userdata C.uintptr_t) {
	state, ok := safeHandleAs[*completionControlCallbackState](userdata)
	if !ok {
		discardParts(parts, partCount)
		return
	}
	ownedParts, err := takeParts(parts, partCount)
	if err != nil {
		return
	}
	received := &Received{
		routingID: routingIDFromCPtr(sourceRID),
		parts:     ownedParts,
	}
	if state.dispatcher.enqueue(&callbackTask{
		label: "completion-control",
		invoke: func() {
			state.handler(received)
		},
		cleanup: func() {
			_ = received.Close()
		},
	}) {
		return
	}
	_ = received.Close()
}
