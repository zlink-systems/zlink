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
		{name: "DEALER typed receive", symbol: "zlink_dealer_recv_part", owner: "func (s *DealerSocket) Recv"},
		{name: "direct retained receive", symbol: "zlink_recv_part_with_hwm_budget_lease", owner: "func (s *directSocket) RecvRetained"},
		{name: "DEALER retained receive", symbol: "zlink_dealer_recv_part_with_hwm_budget_lease", owner: "func (s *DealerSocket) RecvRetained"},
		{name: "ROUTER retained receive", symbol: "zlink_router_recv_part_v2_with_hwm_budget_lease", owner: "func (s *routedSocket) RecvRetained"},
		{name: "SUB retained receive", symbol: "zlink_subscribe_part_with_hwm_budget_lease", owner: "func (s *subscribeSocket) SubscribeRetained"},
		{name: "retained credit release", symbol: "zlink_hwm_budget_lease_release", owner: "func (o *hwmBudgetLeaseOwner) release"},
		{name: "DEALER request reply", symbol: "zlink_dealer_reply_part", owner: "func (s *DealerSocket) reply"},
	} {
		if !strings.Contains(text, capability.symbol) {
			t.Fatalf("Core capability %s is not referenced by Go binding", capability.name)
		}
		if !strings.Contains(text, capability.owner) {
			t.Fatalf("Core capability %s has no typed owner %s", capability.name, capability.owner)
		}
	}
}
