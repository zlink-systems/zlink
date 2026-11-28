package perf

import (
	"os/exec"
	"strings"
	"testing"
)

func TestPublicModuleExcludesPerfDependency(t *testing.T) {
	cmd := exec.Command("go", "-C", "..", "list", "-m", "-mod=readonly", "all")
	cmd.Env = append(cmd.Environ(), "GOWORK=off")
	output, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("list public module dependencies: %v\n%s", err, output)
	}
	for _, module := range strings.Split(string(output), "\n") {
		if path, _, _ := strings.Cut(module, " "); path == "github.com/gorilla/websocket" {
			t.Fatalf("public module includes perf dependency: %s", module)
		}
	}
}
