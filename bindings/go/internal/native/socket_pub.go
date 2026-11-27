// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"
*/
import "C"

type PubSocket struct {
	*publishSocket
}

func newPubSocket(ctx *Context, socketType C.zlink_socket_type_t) (*PubSocket, error) {
	core, err := newSocketCore(ctx, socketType)
	if err != nil {
		return nil, err
	}
	return &PubSocket{
		publishSocket: &publishSocket{connectionSocket: &connectionSocket{socketCore: core}},
	}, nil
}

func (s *PubSocket) Publish(topic string) PublishOp {
	return newPublishBuilder(func(parts []sendBuilderPart, flags SendFlags) error {
		return s.withCString(topic, func(cstr *C.char) error {
			return submitMultipartFromBuilderParts(parts, func(native *C.zlink_msg_t, count C.size_t) error {
				nativeResult0, nativeErr0 := C.zlink_publish(s.raw(), cstr, native, count, C.zlink_send_flags_t(flags))
				return submitErrorFromCall(nativeResult0, nativeErr0)
			})
		})
	})
}
