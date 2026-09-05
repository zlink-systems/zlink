// SPDX-License-Identifier: MPL-2.0

package native

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
