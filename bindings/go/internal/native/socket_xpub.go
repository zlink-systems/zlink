// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"
*/
import "C"

type XPubSocket struct {
	*xpubSubscribeSocket
}

func newXPubSocket(ctx *Context) (*XPubSocket, error) {
	pub, err := newPubSocket(ctx, C.ZLINK_SOCKET_XPUB)
	if err != nil {
		return nil, err
	}
	return &XPubSocket{xpubSubscribeSocket: &xpubSubscribeSocket{publishSocket: pub.publishSocket}}, nil
}

func (s *XPubSocket) Publish(topic string) PublishOp {
	return newPublishBuilder(func(parts []sendBuilderPart, flags SendFlags) error {
		return s.withCString(topic, func(cstr *C.char) error {
			return submitMultipartFromBuilderParts(parts, func(native *C.zlink_msg_t, count C.size_t) error {
				nativeResult15, nativeErr15 := C.zlink_publish(s.raw(), cstr, native, count, C.zlink_send_flags_t(flags))
				return submitErrorFromCall(nativeResult15, nativeErr15)
			})
		})
	})
}
