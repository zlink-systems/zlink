package native

import (
	"encoding/json"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

type hotPathCostInventory struct {
	Schema       int      `json:"schema"`
	Binding      string   `json:"binding"`
	Unclassified []string `json:"unclassified"`
	Entries      []struct {
		ID             string   `json:"id"`
		Classification string   `json:"classification"`
		Sources        []string `json:"sources"`
		Reason         string   `json:"reason"`
		GuardTest      string   `json:"guardTest"`
	} `json:"entries"`
}

func TestHotPathCostInventoryIsClassified(t *testing.T) {
	root := moduleRoot(t)
	body, err := os.ReadFile(filepath.Join(root, "tests", "hot-path-cost-inventory.json"))
	if err != nil {
		t.Fatal(err)
	}
	var inventory hotPathCostInventory
	if err := json.Unmarshal(body, &inventory); err != nil {
		t.Fatal(err)
	}
	if inventory.Schema != 1 || inventory.Binding != "go" {
		t.Fatalf("unexpected hot-path inventory identity: schema=%d binding=%q", inventory.Schema, inventory.Binding)
	}
	if len(inventory.Unclassified) != 0 {
		t.Fatalf("hot-path inventory has unclassified costs: %v", inventory.Unclassified)
	}

	testFuncs := declaredTestFuncs(t, root)
	wantIDs := map[string]struct{}{
		"message-native-allocation":    {},
		"message-snapshot-copy":        {},
		"submit-copy-for-preservation": {},
		"managed-send-retry-snapshot":  {},
		"received-adopt-wrapper":       {},
		"topic-buffer-reuse":           {},
		"bytes-builder-native-copy":    {},
		"option-scratch-buffer":        {},
		"completion-owner-queue":       {},
		"cgo-package-runtime-rpath":    {},
	}
	gotIDs := make(map[string]struct{}, len(inventory.Entries))
	for _, entry := range inventory.Entries {
		if entry.ID == "" || entry.Reason == "" || entry.GuardTest == "" {
			t.Fatalf("hot-path entry is missing identity, reason, or guard test: %+v", entry)
		}
		if entry.Classification != "required" {
			t.Fatalf("hot-path entry %s has unsupported classification %q", entry.ID, entry.Classification)
		}
		if _, duplicate := gotIDs[entry.ID]; duplicate {
			t.Fatalf("hot-path entry is duplicated: %s", entry.ID)
		}
		gotIDs[entry.ID] = struct{}{}
		if !testFuncs[entry.GuardTest] {
			t.Fatalf("hot-path entry %s names guard test %q that does not exist", entry.ID, entry.GuardTest)
		}
		if len(entry.Sources) == 0 {
			t.Fatalf("hot-path entry %s has no source", entry.ID)
		}
		for _, source := range entry.Sources {
			path := strings.SplitN(source, ":", 2)[0]
			if _, err := os.Stat(filepath.Join(root, path)); err != nil {
				t.Fatalf("hot-path entry %s refers to missing source %q: %v", entry.ID, source, err)
			}
		}
	}
	if len(gotIDs) != len(wantIDs) {
		t.Fatalf("hot-path inventory entry count = %d, want %d", len(gotIDs), len(wantIDs))
	}
	for id := range wantIDs {
		if _, ok := gotIDs[id]; !ok {
			t.Fatalf("required hot-path entry is missing: %s", id)
		}
	}
}

var testFuncDecl = regexp.MustCompile(`(?m)^func (Test\w+)\(`)

func declaredTestFuncs(t *testing.T, root string) map[string]bool {
	t.Helper()
	names := map[string]bool{}
	err := filepath.WalkDir(root, func(path string, entry fs.DirEntry, err error) error {
		if err != nil || entry.IsDir() || !strings.HasSuffix(path, "_test.go") {
			return err
		}
		body, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		for _, match := range testFuncDecl.FindAllSubmatch(body, -1) {
			names[string(match[1])] = true
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	return names
}

// TestCgoPackageRuntimeRpathIsDeclared guards the cgo-package-runtime-rpath
// inventory entry: every non-Windows target links the runtime shipped beside
// the module and records where to load it.
func TestCgoPackageRuntimeRpathIsDeclared(t *testing.T) {
	body, err := os.ReadFile(filepath.Join(bindingRoot(t), "ffi.go"))
	if err != nil {
		t.Fatal(err)
	}
	checked := 0
	for _, line := range strings.Split(string(body), "\n") {
		if !strings.HasPrefix(line, "#cgo ") || !strings.Contains(line, "LDFLAGS") || strings.Contains(line, "windows") {
			continue
		}
		checked++
		if !strings.Contains(line, "-lzlink") || !strings.Contains(line, "-Wl,-rpath,${SRCDIR}/../../native/") {
			t.Fatalf("cgo LDFLAGS line lacks the module-relative runtime rpath: %s", line)
		}
	}
	if checked != 4 {
		t.Fatalf("checked %d non-Windows LDFLAGS lines, want 4", checked)
	}
}
