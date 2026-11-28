package perf

import (
	"os/exec"
	"testing"
)

// The Go adapter loads the shared Python renderer by file path; the renderer
// must still find its sibling perf_metrics module (#6063 broke this).
func TestPerfReportAdapterLoadsSharedRenderer(t *testing.T) {
	output, err := exec.Command("python3", "perf_report.py", "--help").CombinedOutput()
	if err != nil {
		t.Fatalf("perf_report.py --help: %v\n%s", err, output)
	}
}
