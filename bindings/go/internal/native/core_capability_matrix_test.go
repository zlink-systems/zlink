package native

import (
	"os"
	"strings"
	"testing"
)

// The raw symbol allowlist proves that referenced C declarations are legal.
// This matrix additionally records the Core capabilities that must not become
// silently unbound when a new typed C entry point is added to the package.
func TestCoreCapabilityMatrixHasTypedGoOwner(t *testing.T) {
	var source strings.Builder
	for _, path := range implementationGoFiles(t) {
		body, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		source.Write(body)
		source.WriteByte('\n')
	}
	text := source.String()
	for _, capability := range []struct {
		name   string
		symbol string
		owner  string
	}{
		{name: "completion drain", symbol: "zlink_completion_recv", owner: "func (o *completionOwner) drain"},
		{name: "unified request", symbol: "zlink_request_part", owner: "func submitCompletionRequest"},
		{name: "opaque reply", symbol: "zlink_reply_part", owner: "func (s *routedSocket) reply"},
		{name: "STREAM packet pull", symbol: "zlink_stream_recv_packet", owner: "func (s *StreamSocket) RecvPacket"},
	} {
		if !strings.Contains(text, capability.symbol) {
			t.Fatalf("Core capability %s is not referenced by Go binding", capability.name)
		}
		if !strings.Contains(text, capability.owner) {
			t.Fatalf("Core capability %s has no typed owner %s", capability.name, capability.owner)
		}
	}
}
