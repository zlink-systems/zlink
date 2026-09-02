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
	clonedParts, err := recvMultipart(reuse, flags, func(part *C.zlink_msg_t, hasMore *C.zlink_part_flag_t, recvFlags C.zlink_recv_flags_t) error {
		return recvErrorFromResult(C.zlink_recv_part(s.raw(), &sourceRID, part, hasMore, recvFlags))
	})
	if err != nil {
		if isNoData(err) {
			return false, nil
		}
		return false, err
	}
	out.replace(routingIDFromCPtr(sourceRID), clonedParts, ReplyToken{}, nil, nil)
	return true, nil
}
