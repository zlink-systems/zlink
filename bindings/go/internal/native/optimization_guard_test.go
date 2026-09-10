package native

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

var requiredWholeMessageSymbols = []string{
	"zlink_send",
	"zlink_send_rid",
	"zlink_recv",
	"zlink_publish",
	"zlink_subscribe",
	"zlink_router_recv",
	"zlink_request",
	"zlink_reply",
	"zlink_xpub_recv",
	"zlink_completion_recv",
	"zlink_completion_close",
	"zlink_stream_recv_packet",
}

func TestRequestUsesUnifiedCoreTargetContract(t *testing.T) {
	path := filepath.Join(bindingRoot(t), "dealer_router_request.go")
	bodyBytes, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	body := string(bodyBytes)

	if !strings.Contains(body, "C.zlink_go_request_with_context(") ||
		!strings.Contains(body, "return zlink_request(") {
		t.Fatal("send/request adapter must use the uintptr-safe whole-message zlink_request wrapper")
	}
}

func TestCompletionCleanupHasOneNativeCloseSite(t *testing.T) {
	path := filepath.Join(bindingRoot(t), "completion_owner.go")
	bodyBytes, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if got := strings.Count(string(bodyBytes), "C.zlink_completion_close("); got != 1 {
		t.Fatalf("native completion close call sites = %d, want 1 guarded site", got)
	}
}

func bindingRoot(t *testing.T) string {
	t.Helper()
	_, file, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("runtime.Caller failed")
	}
	return filepath.Dir(file)
}

func moduleRoot(t *testing.T) string {
	t.Helper()
	return filepath.Clean(filepath.Join(bindingRoot(t), "..", ".."))
}

func implementationGoFiles(t *testing.T) []string {
	t.Helper()
	root := bindingRoot(t)
	var files []string
	err := filepath.WalkDir(root, func(path string, entry os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if entry.IsDir() {
			name := entry.Name()
			if name == "perf" || name == "samples" || name == "tests" || name == "codec" {
				return filepath.SkipDir
			}
			return nil
		}
		if strings.HasSuffix(path, ".go") && !strings.HasSuffix(path, "_test.go") {
			files = append(files, path)
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	return files
}

func goFilesUnder(t *testing.T, roots ...string) []string {
	t.Helper()
	var files []string
	for _, root := range roots {
		err := filepath.WalkDir(root, func(path string, entry os.DirEntry, err error) error {
			if err != nil {
				return err
			}
			if entry.IsDir() {
				if strings.Contains(path, string(filepath.Separator)+"build") ||
					strings.Contains(path, string(filepath.Separator)+"results") {
					return filepath.SkipDir
				}
				return nil
			}
			if strings.HasSuffix(path, ".go") {
				files = append(files, path)
			}
			return nil
		})
		if err != nil {
			t.Fatal(err)
		}
	}
	return files
}

func TestOptimizationGuardUsesWholeMessageSubstrate(t *testing.T) {
	files := implementationGoFiles(t)
	var all strings.Builder
	for _, path := range files {
		body, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		all.Write(body)
		all.WriteByte('\n')
	}
	source := all.String()
	for _, symbol := range requiredWholeMessageSymbols {
		if !strings.Contains(source, symbol) {
			t.Fatalf("missing required helper substrate symbol %s", symbol)
		}
	}

	var violations []string
	part := "part"
	for _, path := range files {
		bodyBytes, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		body := string(bodyBytes)
		for _, token := range []string{"_" + part + "_with_context", "_" + part + "_rid", "_" + part + "(", "ZLINK_" + strings.ToUpper(part) + "_", "zlink_" + part + "_flag_t"} {
			if strings.Contains(body, token) {
				violations = append(violations, filepath.Base(path)+":"+token)
			}
		}
	}
	if len(violations) != 0 {
		t.Fatalf("removed part API references found: %v", violations)
	}
}

func TestOptimizationGuardAvoidsRuntimeFinalizersAndSleeps(t *testing.T) {
	for _, path := range implementationGoFiles(t) {
		bodyBytes, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		body := string(bodyBytes)
		if strings.Contains(body, "runtime.SetFinalizer") {
			t.Fatalf("%s uses runtime.SetFinalizer; explicit close owns lifecycle", path)
		}
		if strings.Contains(body, "time.Sleep(") {
			t.Fatalf("%s uses time.Sleep in binding implementation hot path", path)
		}
	}
}

func TestSamplesAndPerfUseRootPublicContract(t *testing.T) {
	root := moduleRoot(t)
	var violations []string
	for _, path := range goFilesUnder(t, filepath.Join(root, "samples"), filepath.Join(root, "perf")) {
		bodyBytes, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		body := string(bodyBytes)
		for _, token := range []string{
			`"zlink.systems/zlink/internal/native"`,
			`"zlink.systems/zlink/contracts"`,
		} {
			if strings.Contains(body, token) {
				violations = append(violations, filepath.Base(path)+":"+token)
			}
		}
	}
	if len(violations) != 0 {
		t.Fatalf("samples/perf must use the root public contract: %v", violations)
	}
}
