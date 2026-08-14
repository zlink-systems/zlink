// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <errno.h>
#include "zlink.h"
*/
import "C"

import "errors"

func submitBackpressureResult(err error) (bool, error) {
	if err == nil {
		return true, nil
	}
	var submitErr *SubmitError
	if errors.As(err, &submitErr) && submitErr.Result == SubmitBackpressured {
		return false, nil
	}
	return false, err
}

func cloneParts(parts []*Message) ([]*Message, error) {
	n := len(parts)
	if n == 0 {
		return nil, &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	// Pre-size the result and assign by index. The previous append-based
	// loop incurred bounds-check + len-increment work on every iteration;
	// since we know n up front, neither is needed. Hot path for request,
	// send-to-peer, and routed reply.
	cloned := make([]*Message, n)
	for i := 0; i < n; i++ {
		part := parts[i]
		if part == nil {
			closeMessageSlice(cloned[:i])
			return nil, &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
		}
		dup, err := part.clone()
		if err != nil {
			closeMessageSlice(cloned[:i])
			return nil, err
		}
		cloned[i] = dup
	}
	return cloned, nil
}
