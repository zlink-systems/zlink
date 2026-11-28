// SPDX-License-Identifier: MPL-2.0

package native

import "testing"

func TestRoutedReceiveMetadataRequiresNoAllocation(t *testing.T) {
	socket := &routedSocket{connectionSocket: &connectionSocket{socketCore: &socketCore{}}, replyOwner: &replyTokenOwner{}}
	out := &Received{}
	rid := NewRoutingIDString("peer")
	parts := []*Message{{}}
	for _, token := range []uint64{0, 42} {
		got := testing.AllocsPerRun(100, func() {
			socket.replaceRoutedReceived(out, rid, parts, token, 7)
		})
		if got != 0 {
			t.Errorf("token %d: got %g allocations, want 0", token, got)
		}
		if out.RoutingID() != rid || out.RouteGeneration() != 7 {
			t.Fatal("received routing metadata was lost")
		}
		if got, ok := out.ReplyToken(); ok != (token != 0) || got.value != token {
			t.Fatalf("reply token = %v, %t, want token value %d", got, ok, token)
		}
	}
}
