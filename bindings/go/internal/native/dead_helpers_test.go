// SPDX-License-Identifier: MPL-2.0

package native

import (
	"go/parser"
	"go/token"
	"testing"
)

func TestUnusedNativeHelpersAreAbsent(t *testing.T) {
	for _, path := range implementationGoFiles(t) {
		file, err := parser.ParseFile(token.NewFileSet(), path, nil, 0)
		if err != nil {
			t.Fatal(err)
		}
		for _, name := range []string{"takeParts", "discardParts", "submitBackpressureResult", "monitorEventFromRawFieldsForTest"} {
			if file.Scope.Lookup(name) != nil {
				t.Errorf("%s declares %s in production", path, name)
			}
		}
	}
}
