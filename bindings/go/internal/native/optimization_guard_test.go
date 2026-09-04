package native

import (
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"testing"
)

var aggregateHotPathSymbols = []string{
	"zlink_send",
	"zlink_recv",
	"zlink_publish",
	"zlink_subscribe",
	"zlink_router_recv",
	"zlink_dealer_request",
	"zlink_router_request",
	"zlink_router_reply",
}

var requiredPartSymbols = []string{
	"zlink_send_part",
	"zlink_recv_part",
	"zlink_publish_part",
	"zlink_subscribe_part",
	"zlink_router_recv_part",
	"zlink_send_part_rid",
	"zlink_request_part",
	"zlink_reply_part",
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

	if !strings.Contains(body, "C.zlink_go_request_part_with_context(") ||
		!strings.Contains(body, "return zlink_request_part(") {
		t.Fatal("send/request adapter must use the uintptr-safe zlink_request_part wrapper")
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

func TestOptimizationGuardUsesPartSubstrate(t *testing.T) {
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
	for _, symbol := range requiredPartSymbols {
		if !strings.Contains(source, symbol) {
			t.Fatalf("missing required helper substrate symbol %s", symbol)
		}
	}

	var violations []string
	for _, path := range files {
		bodyBytes, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		body := string(bodyBytes)
		for _, symbol := range aggregateHotPathSymbols {
			pattern := regexp.MustCompile(`\bC\.` + regexp.QuoteMeta(symbol) + `\s*\(`)
			for _, loc := range pattern.FindAllStringIndex(body, -1) {
				if strings.HasPrefix(body[loc[0]:], "C."+symbol+"_part") {
					continue
				}
				violations = append(violations, filepath.Base(path)+":"+symbol)
			}
		}
	}
	if len(violations) != 0 {
		t.Fatalf("aggregate hot-path calls found: %v", violations)
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
