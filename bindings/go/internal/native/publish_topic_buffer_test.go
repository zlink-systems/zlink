// SPDX-License-Identifier: MPL-2.0

package native

import (
	"os"
	"strings"
	"testing"
)

func TestPublishUsesOneGoOwnedTopicBufferPath(t *testing.T) {
	calls := 0
	for _, path := range implementationGoFiles(t) {
		source, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		text := string(source)
		count := strings.Count(text, "C.zlink_publish(")
		calls += count
		if count != 0 && (strings.Contains(text, ".withCString(") || strings.Contains(text, "C.CString(")) {
			t.Errorf("%s publishes through a C-allocated string", path)
		}
	}
	if calls != 1 {
		t.Errorf("found %d native publish call sites, want 1 shared owner", calls)
	}
}
