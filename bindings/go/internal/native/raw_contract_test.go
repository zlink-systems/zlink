package native

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"testing"
)

type rawCore11Allowlist struct {
	Schema      int    `json:"schema"`
	CoreVersion string `json:"coreVersion"`
	Headers     []struct {
		Path   string `json:"path"`
		SHA256 string `json:"sha256"`
	} `json:"headers"`
	PublicSymbols   []string `json:"publicSymbols"`
	LocalSymbols    []string `json:"localSymbols"`
	ForbiddenPaths  []string `json:"forbiddenPaths"`
	ForbiddenTokens []string `json:"forbiddenTokens"`
	FFI             struct {
		CFlags                    string `json:"cflags"`
		UsesRepositoryCoreInclude bool   `json:"usesRepositoryCoreInclude"`
	} `json:"ffi"`
}

func TestRawCore11AllowlistMatchesHeadersAndCgo(t *testing.T) {
	root := moduleRoot(t)
	allowlistPath := filepath.Join(root, "tests", "raw-core11-allowlist.json")
	body, err := os.ReadFile(allowlistPath)
	if err != nil {
		t.Fatal(err)
	}
	var allowlist rawCore11Allowlist
	if err := json.Unmarshal(body, &allowlist); err != nil {
		t.Fatal(err)
	}
	if allowlist.Schema != 1 || allowlist.CoreVersion != "11.1.0" {
		t.Fatalf("unexpected raw Core allowlist identity: schema=%d version=%q", allowlist.Schema, allowlist.CoreVersion)
	}

	includeRoot := filepath.Join(root, "include")
	actualHeaders := make(map[string]string)
	var headerText strings.Builder
	err = filepath.WalkDir(includeRoot, func(path string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if entry.IsDir() {
			return nil
		}
		if !entry.Type().IsRegular() {
			t.Fatalf("non-regular file in public header tree: %s", path)
		}
		content, readErr := os.ReadFile(path)
		if readErr != nil {
			return readErr
		}
		rel, relErr := filepath.Rel(root, path)
		if relErr != nil {
			return relErr
		}
		rel = filepath.ToSlash(rel)
		hash := sha256.Sum256(content)
		actualHeaders[rel] = hex.EncodeToString(hash[:])
		headerText.Write(content)
		headerText.WriteByte('\n')
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}

	expectedHeaders := make(map[string]string, len(allowlist.Headers))
	for _, header := range allowlist.Headers {
		expectedHeaders[header.Path] = header.SHA256
	}
	if got, want := sortedKeys(actualHeaders), sortedKeys(expectedHeaders); !sameStrings(got, want) {
		t.Fatalf("public header set changed: got=%v want=%v", got, want)
	}
	for path, want := range expectedHeaders {
		if got := actualHeaders[path]; got != want {
			t.Fatalf("public header hash changed for %s: got=%s want=%s", path, got, want)
		}
	}

	for _, forbidden := range allowlist.ForbiddenPaths {
		if _, err := os.Stat(filepath.Join(root, filepath.FromSlash(forbidden))); err == nil {
			t.Fatalf("forbidden raw header path exists: %s", forbidden)
		} else if !os.IsNotExist(err) {
			t.Fatal(err)
		}
	}
	for _, forbidden := range allowlist.ForbiddenTokens {
		if strings.Contains(headerText.String(), forbidden) {
			t.Fatalf("forbidden Core 10/service token remains in headers: %s", forbidden)
		}
	}

	ffi, err := os.ReadFile(filepath.Join(root, "internal", "native", "ffi.go"))
	if err != nil {
		t.Fatal(err)
	}
	ffiText := string(ffi)
	if !strings.Contains(ffiText, allowlist.FFI.CFlags) {
		t.Fatalf("ffi.go does not use the package-local include path %q", allowlist.FFI.CFlags)
	}
	if allowlist.FFI.UsesRepositoryCoreInclude || strings.Contains(ffiText, "core/include") {
		t.Fatalf("ffi.go must not depend on repository core/include")
	}

	var source strings.Builder
	for _, path := range implementationGoFiles(t) {
		content, readErr := os.ReadFile(path)
		if readErr != nil {
			t.Fatal(readErr)
		}
		source.Write(content)
		source.WriteByte('\n')
	}
	allSymbols := regexp.MustCompile(`\bC\.(zlink_[A-Za-z0-9_]+)\b`).FindAllStringSubmatch(source.String(), -1)
	actualPublic := make(map[string]struct{})
	actualLocal := make(map[string]struct{})
	localSet := make(map[string]struct{}, len(allowlist.LocalSymbols))
	for _, symbol := range allowlist.LocalSymbols {
		localSet[symbol] = struct{}{}
	}
	for _, match := range allSymbols {
		symbol := match[1]
		if _, ok := localSet[symbol]; ok {
			actualLocal[symbol] = struct{}{}
		} else {
			actualPublic[symbol] = struct{}{}
		}
	}
	if got, want := sortedSet(actualPublic), sortedStrings(allowlist.PublicSymbols); !sameStrings(got, want) {
		t.Fatalf("raw C symbol inventory changed: got=%v want=%v", got, want)
	}
	if got, want := sortedSet(actualLocal), sortedStrings(allowlist.LocalSymbols); !sameStrings(got, want) {
		t.Fatalf("local cgo helper inventory changed: got=%v want=%v", got, want)
	}
	for _, symbol := range allowlist.PublicSymbols {
		if !strings.Contains(headerText.String(), symbol) {
			t.Fatalf("cgo symbol is not declared by the packaged raw headers: %s", symbol)
		}
	}
	for _, symbol := range allowlist.LocalSymbols {
		if !strings.Contains(source.String(), "static inline") || !strings.Contains(source.String(), symbol) {
			t.Fatalf("local cgo helper is not defined in the binding bridge: %s", symbol)
		}
	}
}

func sortedKeys(values map[string]string) []string {
	keys := make([]string, 0, len(values))
	for key := range values {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	return keys
}

func sortedSet(values map[string]struct{}) []string {
	result := make([]string, 0, len(values))
	for value := range values {
		result = append(result, value)
	}
	sort.Strings(result)
	return result
}

func sortedStrings(values []string) []string {
	result := append([]string(nil), values...)
	sort.Strings(result)
	return result
}

func sameStrings(left, right []string) bool {
	if len(left) != len(right) {
		return false
	}
	for i := range left {
		if left[i] != right[i] {
			return false
		}
	}
	return true
}
