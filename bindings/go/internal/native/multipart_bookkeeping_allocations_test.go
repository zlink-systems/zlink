// SPDX-License-Identifier: MPL-2.0

package native

import "testing"

func TestMultipartPreparationReusesBuilderPartsForRollback(t *testing.T) {
	parts := []sendBuilderPart{
		{bytes: true, data: []byte("first")},
		{message: nil},
	}
	var rejected error
	allocations := testing.AllocsPerRun(100, func() {
		rejected = submitMultipartFromBuilderParts(parts, nil)
	})
	if config, ok := rejected.(*ConfigError); !ok || config.Result != ConfigInvalidArgument {
		t.Fatalf("multipart preparation error = %v, want invalid argument", rejected)
	}
	if allocations != 2 {
		t.Fatalf("multipart preparation allocated %g Go objects, want only the native array and error", allocations)
	}
}
