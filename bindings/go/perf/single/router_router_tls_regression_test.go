package main

import (
	"context"
	"os/exec"
	"strings"
	"testing"
	"time"
)

func TestRouterRouterTLSSinglePerfDoesNotAbort(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	cmd := exec.CommandContext(
		ctx,
		"go",
		"run",
		".",
		"--pattern", "ROUTER_ROUTER",
		"--transport", "tls",
		"--msg-size", "64",
		"--duration", "2",
	)
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
