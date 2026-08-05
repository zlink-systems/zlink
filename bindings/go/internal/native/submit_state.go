// SPDX-License-Identifier: MPL-2.0

package native

import (
	"errors"
	"syscall"
)

type submitOnce struct {
	submitted bool
}

func (s *submitOnce) markSubmitted() error {
	if s.submitted {
		return configInvalidStateError()
	}
	s.submitted = true
	return nil
}

func configInvalidStateError() error {
	return &ConfigError{Result: ConfigInvalidState, nativeErrno: int(syscall.EINVAL)}
}

func configInvalidArgumentError() error {
	return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(syscall.EINVAL)}
}

func submitBackpressureAsNotSubmitted(err error) (bool, error) {
	var submitErr *SubmitError
	if errors.As(err, &submitErr) && submitErr.Result == SubmitBackpressured {
		return false, nil
	}
	return false, err
}
