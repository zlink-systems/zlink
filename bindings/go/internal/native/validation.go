// SPDX-License-Identifier: MPL-2.0

package native

import "strings"

func validateCString(value string) error {
	if strings.IndexByte(value, 0) >= 0 {
		return validationError("value contains null byte")
	}
	return nil
}

func validateEndpointString(value string) error {
	if err := validateCString(value); err != nil {
		return validationError("endpoint contains null byte")
	}
	if len(value) > maxFixedCStringFieldSize {
		return validationError("endpoint is too long")
	}
	return nil
}
