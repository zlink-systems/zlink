// SPDX-License-Identifier: MPL-2.0

package native

import "strings"

func validateEndpointString(value string) error {
	if strings.IndexByte(value, 0) >= 0 {
		return validationError("endpoint contains null byte")
	}
	if len(value) > maxFixedCStringFieldSize {
		return validationError("endpoint is too long")
	}
	return nil
}
