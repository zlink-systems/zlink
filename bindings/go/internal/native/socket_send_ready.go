// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"

extern void goZlinkSendReadyTrampoline(void *subject_, uintptr_t userdata_);

static inline int zlink_send_ready_handler_go_local(void *s, uintptr_t userdata) {
    return zlink_send_ready_handler(s, (zlink_send_ready_handler_fn)goZlinkSendReadyTrampoline, (void *)userdata);
}
*/
import "C"

import "runtime/cgo"

func (s *connectionSocket) setSendReady(handler func()) error {
	if handler == nil {
		return &HandlerError{Result: HandlerInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	state := newSendReadyCallbackState(sendReadyCallback(handler))
	handle := cgo.NewHandle(state)
	err := s.socketCore.replaceCallback(handle, &s.sendReadyHandle, nil, func() error {
		return handlerErrorFromResult(C.zlink_send_ready_handler_go_local(s.raw(), C.uintptr_t(handle)))
	})
	if err != nil {
		state.close()
		handle.Delete()
		return err
	}
	return nil
}
