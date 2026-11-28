// SPDX-License-Identifier: MPL-2.0

package native

import (
	"errors"
	"strings"
	"syscall"
	"testing"
)

func TestBindingConfigErrorsPreserveDiagnostic(t *testing.T) {
	for _, tc := range []struct {
		err     error
		result  ConfigResult
		errno   syscall.Errno
		message string
	}{
		{validationError("invalid flag %d", 42), ConfigInvalidArgument, syscall.EINVAL, "invalid flag 42"},
		{stateError("closed %s", "socket"), ConfigInvalidHandle, syscall.EFAULT, "closed socket"},
	} {
		var config *ConfigError
		if !errors.As(tc.err, &config) || config.Result != tc.result || !errors.Is(tc.err, tc.errno) {
			t.Errorf("%v lost its config result or errno", tc.err)
		}
		if !strings.Contains(tc.err.Error(), tc.message) {
			t.Errorf("%q lacks %q", tc.err, tc.message)
		}
	}
}
