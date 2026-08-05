package zlink_test

import (
	"context"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

func TestRouterRouterTLSSinglePerfDoesNotAbort(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	_, thisFile, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("failed to resolve test source path")
	}
	repoDir := filepath.Dir(thisFile)

	cmd := exec.CommandContext(
		ctx,
		"go",
		"run",
		"./perf/single",
		"--pattern", "ROUTER_ROUTER",
		"--transport", "tls",
		"--msg-size", "64",
		"--duration", "2",
	)
	cmd.Dir = repoDir

	out, err := cmd.CombinedOutput()
	if ctx.Err() == context.DeadlineExceeded {
		t.Fatalf("router/router tls perf timed out: %s", string(out))
	}
	if err != nil {
		t.Fatalf("router/router tls perf failed: %v\n%s", err, string(out))
	}

	output := string(out)
	if strings.Contains(output, "SIGABRT") || strings.Contains(output, "Bad address") {
		t.Fatalf("router/router tls perf aborted:\n%s", output)
	}
	if !strings.Contains(output, "RESULT,current,ROUTER_ROUTER,tls,64,throughput,") {
		t.Fatalf("router/router tls perf did not produce result line:\n%s", output)
	}
}
