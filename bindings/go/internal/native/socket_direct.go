// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "zlink.h"
*/
import "C"

type directSocket struct {
	*connectionSocket
}

func (s *directSocket) Recv(out *Received, flags RecvFlags) (bool, error) {
	if out == nil {
		return false, &RecvError{Result: RecvInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	reuse := out.beginReceive()
	var sourceRID *C.zlink_routing_id_t
	parts, err := recvMultipart(&out.nativeParts, reuse, flags, func(native *C.zlink_msg_t, capacity C.size_t, count *C.size_t, recvFlags C.zlink_recv_flags_t) C.zlink_recv_result_t {
		return C.zlink_recv(s.raw(), &sourceRID, native, capacity, count, recvFlags)
	})
	if err != nil {
		if isNoData(err) {
			return false, nil
		}
		return false, err
	}
	out.replace(routingIDFromCPtr(sourceRID), parts, ReplyToken{}, nil, nil)
	return true, nil
}
