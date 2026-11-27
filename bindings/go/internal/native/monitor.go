// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include "zlink.h"
*/
import "C"

import (
	"sync"
	"sync/atomic"
	"unsafe"
	"zlink.systems/zlink/contracts"
)

type MonitorEventMask = contracts.MonitorEventMask
type MonitorSourceKind = contracts.MonitorSourceKind
type MonitorEventType = contracts.MonitorEventType
type MonitorEventFlag = contracts.MonitorEventFlag
type MonitorTransportLane = contracts.MonitorTransportLane
type MonitorOpenOption = contracts.MonitorOpenOption

type MonitorEvent = contracts.MonitorEvent
type MonitorStatus = contracts.MonitorStatus

func monitorStatusFromC(raw C.zlink_monitor_status_t) MonitorStatus {
	return MonitorStatus{
		ABIVersion:                       uint32(raw.abi_version),
		StructSize:                       uint32(raw.struct_size),
		SourceKind:                       MonitorSourceKind(raw.source_kind),
		StateFlags:                       uint32(raw.state_flags),
		DetailFlags:                      uint32(raw.detail_flags),
		SndPendingMsgs:                   uint64(raw.snd_pending_msgs),
		RcvPendingMsgs:                   uint64(raw.rcv_pending_msgs),
		SndPendingBytes:                  uint64(raw.snd_pending_bytes),
		RcvPendingBytes:                  uint64(raw.rcv_pending_bytes),
		AutoHwmEnabled:                   uint32(raw.auto_hwm_enabled) != 0,
		AutoHwmProfile:                   uint32(raw.auto_hwm_profile),
		AutoHwmRole:                      uint32(raw.auto_hwm_role),
		AutoHwmPolicyClass:               uint32(raw.auto_hwm_policy_class),
		AutoHwmPlannedSndHwmBytes:        uint64(raw.auto_hwm_planned_sndhwm_bytes),
		AutoHwmPlannedRcvHwmBytes:        uint64(raw.auto_hwm_planned_rcvhwm_bytes),
		AutoHwmAppliedSndHwmBytes:        uint64(raw.auto_hwm_applied_sndhwm_bytes),
		AutoHwmAppliedRcvHwmBytes:        uint64(raw.auto_hwm_applied_rcvhwm_bytes),
		AutoHwmEffectiveSndBuf:           int32(raw.auto_hwm_effective_sndbuf),
		AutoHwmEffectiveRcvBuf:           int32(raw.auto_hwm_effective_rcvbuf),
		AutoHwmLastRecalcMs:              uint64(raw.auto_hwm_last_recalc_ms),
		AutoHwmLastRecalcReason:          AutoHwmRecalcReason(raw.auto_hwm_last_recalc_reason),
		AutoHwmSendBlockedRatioPPM:       uint32(raw.auto_hwm_send_blocked_ratio_ppm),
		AutoHwmDeferredSndHwmBytes:       uint64(raw.auto_hwm_deferred_sndhwm_bytes),
		AutoHwmDeferredRcvHwmBytes:       uint64(raw.auto_hwm_deferred_rcvhwm_bytes),
		AutoHwmDeferredSndHwmValid:       uint32(raw.auto_hwm_deferred_sndhwm_valid) != 0,
		AutoHwmDeferredRcvHwmValid:       uint32(raw.auto_hwm_deferred_rcvhwm_valid) != 0,
		SndBytesInFlight:                 uint64(raw.snd_bytes_in_flight),
		RcvBytesInFlight:                 uint64(raw.rcv_bytes_in_flight),
		MinimumCoreMessageChargeBytes:    uint64(raw.minimum_core_message_charge_bytes),
		OversizeMessageAdmissionCount:    uint64(raw.oversize_message_admission_count),
		OversizeMessageAdmissionMaxBytes: uint64(raw.oversize_message_admission_max_bytes),
		FlowPausedConnections:            uint64(raw.flow_paused_connections),
		FlowPauseAppliedTotal:            uint64(raw.flow_pause_applied_total),
		FlowResumeAppliedTotal:           uint64(raw.flow_resume_applied_total),
		FlowStateStaleTotal:              uint64(raw.flow_state_stale_total),
		FlowPauseDurationMs:              uint64(raw.flow_pause_duration_ms),
	}
}

type SocketMonitor struct {
	handle  atomic.Pointer[byte]
	closeMu sync.Mutex
}

func OpenSocketMonitor(socket SocketTarget, options ...MonitorOpenOption) (*SocketMonitor, error) {
	if socket == nil {
		return nil, newConfigError(ConfigInvalidArgument, int(C.EINVAL))
	}
	events, monitorHwmBytes, ok := contracts.ResolveMonitorOpenOptions(options)
	if !ok {
		return nil, newConfigError(ConfigInvalidArgument, int(C.EINVAL))
	}
	nativeOptions := C.zlink_socket_monitor_open_options_t{
		events:            C.zlink_socket_monitor_event_mask_t(events),
		monitor_hwm_bytes: C.uint64_t(monitorHwmBytes),
	}
	handle, cerr := C.zlink_socket_monitor_open(socket.raw(), &nativeOptions)
	if handle == nil {
		return nil, configErrorFromErrno(cgoErrno(cerr))
	}
	monitor := &SocketMonitor{}
	monitor.handle.Store((*byte)(handle))
	return monitor, nil
}

func (m *SocketMonitor) raw() unsafe.Pointer {
	if m == nil {
		return nil
	}
	return unsafe.Pointer(m.handle.Load())
}

// Recv returns the next monitor event. Returns (nil, *RecvError{Result:RecvNoData})
// when DONTWAIT finds nothing. Value-return form is allowed for monitor/timer
// control-plane APIs by doc/spec/bindings/go/README.md §Receive And Subscribe Shape.
func (m *SocketMonitor) Recv(flags RecvFlags) (*MonitorEvent, error) {
	handle := m.raw()
	if handle == nil {
		return nil, newRecvError(RecvInvalidHandle, int(C.EFAULT))
	}
	var raw C.zlink_socket_monitor_event_t
	rc, cerr := C.zlink_socket_monitor_recv(handle, &raw, C.zlink_recv_flags_t(flags))
	if err := recvErrorFromCall(rc, cerr); err != nil {
		return nil, err
	}
	return monitorEventFromC(raw), nil
}

func (m *SocketMonitor) Status() (*MonitorStatus, error) {
	handle := m.raw()
	if handle == nil {
		return nil, newConfigError(ConfigInvalidHandle, int(C.EFAULT))
	}
	var raw C.zlink_monitor_status_t
	rc, cerr := C.zlink_monitor_status(handle, &raw)
	if err := configErrorFromCall(rc, cerr); err != nil {
		return nil, err
	}
	snapshot := monitorStatusFromC(raw)
	return &snapshot, nil
}

func (m *SocketMonitor) Close() error {
	if m == nil {
		return nil
	}
	m.closeMu.Lock()
	handlePtr := m.raw()
	if handlePtr == nil {
		m.closeMu.Unlock()
		return nil
	}
	handle := handlePtr
	rc, cerr := C.zlink_monitor_close(&handle)
	if err := closeErrorFromCall(rc, cerr); err != nil {
		m.closeMu.Unlock()
		return err
	}
	m.handle.Store(nil)
	m.closeMu.Unlock()
	return nil
}

func monitorEventFromC(raw C.zlink_socket_monitor_event_t) *MonitorEvent {
	return &MonitorEvent{
		Event:         MonitorEventType(raw.event),
		Value:         uint64(raw.value),
		RoutingID:     routingIDFromC(raw.routing_id),
		LocalAddr:     C.GoString(&raw.local_addr[0]),
		RemoteAddr:    C.GoString(&raw.remote_addr[0]),
		ConnectionID:  uint64(raw.connection_id),
		TransportLane: MonitorTransportLane(raw.transport_lane),
		Flags:         MonitorEventFlag(raw.flags),
	}
}

// monitorEventFromRawFieldsForTest exercises monitorEventFromC's field-by-field
// conversion from plain Go values so tests can verify the 64-bit round-trip
// without importing C from a test compilation unit.
func monitorEventFromRawFieldsForTest(
	eventType MonitorEventType,
	value, connectionID uint64,
	transportLane MonitorTransportLane,
	flags MonitorEventFlag,
) *MonitorEvent {
	var raw C.zlink_socket_monitor_event_t
	raw.event = C.uint64_t(eventType)
	raw.value = C.uint64_t(value)
	raw.connection_id = C.uint64_t(connectionID)
	raw.transport_lane = C.uint32_t(transportLane)
	raw.flags = C.uint32_t(flags)
	return monitorEventFromC(raw)
}
