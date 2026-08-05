// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "zlink.h"

extern void goZlinkRecvTrampoline(zlink_routing_id_t *source_rid_, zlink_msg_t *parts_, size_t part_count_, uintptr_t userdata_);

static inline int zlink_recv_handler_go_local(void *s, uintptr_t userdata) {
    return zlink_recv_handler(s, (zlink_socket_msg_handler_fn)goZlinkRecvTrampoline, (void *)userdata);
}
*/
import "C"

import "runtime/cgo"

type directSocket struct {
	*connectionSocket
}

func (s *directSocket) OnSendReady(handler func()) error {
	return s.setSendReady(handler)
}

func (s *directSocket) submit(flags SendFlags, parts ...*Message) (bool, error) {
	err := submitMultipartFromClones(parts, true, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
		return submitErrorFromResult(C.zlink_send_part(s.raw(), part, C.zlink_send_flags_t(flags), partFlag))
	})
	return submitBackpressureResult(err)
}

func (s *directSocket) Recv(out *Received, flags RecvFlags) (bool, error) {
	if out == nil {
		return false, &RecvError{Result: RecvInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	reuse := out.beginReceive()
	var sourceRID *C.zlink_routing_id_t
	clonedParts, err := recvMultipart(reuse, flags, func(part *C.zlink_msg_t, hasMore *C.zlink_part_flag_t, recvFlags C.zlink_recv_flags_t) error {
		return recvErrorFromResult(C.zlink_recv_part(s.raw(), &sourceRID, part, hasMore, recvFlags))
	})
	if err != nil {
		if isNoData(err) {
			return false, nil
		}
		return false, err
	}
	out.replace(routingIDFromCPtr(sourceRID), clonedParts, 0, false, nil, nil)
	return true, nil
}

func (s *directSocket) onReceive(handler func(*Received)) error {
	if handler == nil {
		return &HandlerError{Result: HandlerInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	state := newRecvCallbackState(recvCallback(handler))
	handle := cgo.NewHandle(state)
	err := s.connectionSocket.replaceCallback(handle, &s.recvHandle, &s.recvActive, func() error {
		return handlerErrorFromResult(C.zlink_recv_handler_go_local(s.raw(), C.uintptr_t(handle)))
	})
	if err != nil {
		state.close()
		handle.Delete()
		return err
	}
	return nil
}
