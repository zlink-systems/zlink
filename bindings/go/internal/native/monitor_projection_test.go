// SPDX-License-Identifier: MPL-2.0

package native

import (
	"testing"
	"unsafe"

	"zlink.systems/zlink/internal/native/monitortest"
)

// Go test files cannot import C. T is the Core event type used by monitorEventFromC.
func projectMonitorFixture[T any](project func(T) *MonitorEvent, event, value, id uint64, lane, flags uint32) *MonitorEvent {
	var result *MonitorEvent
	monitortest.WithEvent(event, value, id, lane, flags, func(raw unsafe.Pointer) {
		result = project(*(*T)(raw))
	})
	return result
}

// §8.1.1 follow-up: monitorEventFromC must preserve the full 64-bit value and
// the connection/flag metadata Core carries alongside a flow-state event,
// without reviving the removed public pair/generation fields.
func TestMonitorEventFromCPreserves64BitValueAndConnectionMetadata(t *testing.T) {
	const bigValue = uint64(0x1_0000_0002)
	const bigConnectionID = uint64(0xFFFF_FFFF_0000_0001)

	event := projectMonitorFixture(monitorEventFromC,
		uint64(MonitorEventTypeFlowStateStale),
		bigValue, bigConnectionID,
		uint32(MonitorTransportLaneCompletion),
		uint32(MonitorEventFlagFlowStateStaleEpoch),
	)

	if event.Value != bigValue {
		t.Fatalf("Value = %#x, want %#x (must not truncate to 32 bits)", event.Value, bigValue)
	}
	if event.ConnectionID != bigConnectionID {
		t.Fatalf("ConnectionID = %#x, want %#x", event.ConnectionID, bigConnectionID)
	}
	if event.TransportLane != MonitorTransportLaneCompletion {
		t.Fatalf("TransportLane = %d, want %d", event.TransportLane, MonitorTransportLaneCompletion)
	}
	if !event.HasFlag(MonitorEventFlagFlowStateStaleEpoch) {
		t.Fatalf("Flags = %#x, missing expected stale-epoch bit", event.Flags)
	}
	if event.HasFlag(MonitorEventFlagConnectionReadyEdge) || event.HasFlag(MonitorEventFlagSendFlowWritable) {
		t.Fatalf("Flags = %#x, unexpected bit set", event.Flags)
	}
	if !event.IsFlowStateStale() {
		t.Fatalf("IsFlowStateStale() = false, want true")
	}
}
