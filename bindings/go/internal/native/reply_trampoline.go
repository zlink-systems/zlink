// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include "zlink.h"
*/
import "C"

import "runtime/cgo"

//export goZlinkReplyTrampoline
func goZlinkReplyTrampoline(result C.zlink_request_result_t, parts *C.zlink_msg_t, partCount C.size_t, userdata C.uintptr_t) {
	handle := cgo.Handle(userdata)
	state, ok := safeHandleAs[*replyCallbackState](userdata)
	if !ok {
		discardParts(parts, partCount)
		return
	}
	defer handle.Delete()
	if result == C.ZLINK_REQUEST_OK {
		clonedParts, err := takeParts(parts, partCount)
		if err != nil {
			state.complete(requestResult{result: RequestProtocolError})
			return
		}
		state.complete(requestResult{result: RequestOK, parts: clonedParts})
		return
	}
	discardParts(parts, partCount)
	state.complete(requestResult{result: RequestResult(result), parts: nil})
}
