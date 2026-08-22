// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include "zlink.h"

extern void goZlinkMonitorTrampoline(zlink_monitor_event_t *event_, uintptr_t userdata_);

static inline int zlink_socket_monitor_handler_go_local(void *m, uintptr_t userdata) {
    return zlink_socket_monitor_handler(m, (zlink_socket_monitor_handler_fn)goZlinkMonitorTrampoline, (void *)userdata);
}

*/
import "C"

import (
	"errors"
	"runtime/cgo"
	"sync"
	"sync/atomic"
	"unsafe"
)

const (
	MonitorEventConnected               MonitorEventMask = MonitorEventMask(C.ZLINK_SOCKET_MONITOR_EVENT_CONNECTED)
	MonitorEventConnectDelayed          MonitorEventMask = MonitorEventMask(C.ZLINK_SOCKET_MONITOR_EVENT_CONNECT_DELAYED)
	MonitorEventConnectRetried          MonitorEventMask = MonitorEventMask(C.ZLINK_SOCKET_MONITOR_EVENT_CONNECT_RETRIED)
	MonitorEventListening               MonitorEventMask = MonitorEventMask(C.ZLINK_SOCKET_MONITOR_EVENT_LISTENING)
	MonitorEventBindFailed              MonitorEventMask = MonitorEventMask(C.ZLINK_SOCKET_MONITOR_EVENT_BIND_FAILED)
	MonitorEventAccepted                MonitorEventMask = MonitorEventMask(C.ZLINK_SOCKET_MONITOR_EVENT_ACCEPTED)
	MonitorEventAcceptFailed            MonitorEventMask = MonitorEventMask(C.ZLINK_SOCKET_MONITOR_EVENT_ACCEPT_FAILED)
	MonitorEventClosed                  MonitorEventMask = MonitorEventMask(C.ZLINK_SOCKET_MONITOR_EVENT_CLOSED)
	MonitorEventCloseFailed             MonitorEventMask = MonitorEventMask(C.ZLINK_SOCKET_MONITOR_EVENT_CLOSE_FAILED)
	MonitorEventDisconnected            MonitorEventMask = MonitorEventMask(C.ZLINK_SOCKET_MONITOR_EVENT_DISCONNECTED)
	MonitorEventMonitorStopped          MonitorEventMask = MonitorEventMask(C.ZLINK_SOCKET_MONITOR_EVENT_MONITOR_STOPPED)
	MonitorEventHandshakeFailedNoDetail MonitorEventMask = MonitorEventMask(C.ZLINK_SOCKET_MONITOR_EVENT_HANDSHAKE_FAILED_NO_DETAIL)
	MonitorEventConnectionReady         MonitorEventMask = MonitorEventMask(C.ZLINK_SOCKET_MONITOR_EVENT_CONNECTION_READY)
	MonitorEventHandshakeFailedProtocol MonitorEventMask = MonitorEventMask(C.ZLINK_SOCKET_MONITOR_EVENT_HANDSHAKE_FAILED_PROTOCOL)
	MonitorEventHandshakeFailedAuth     MonitorEventMask = MonitorEventMask(C.ZLINK_SOCKET_MONITOR_EVENT_HANDSHAKE_FAILED_AUTH)
	MonitorEventPeerWeightChanged       MonitorEventMask = MonitorEventMask(C.ZLINK_SOCKET_MONITOR_EVENT_PEER_WEIGHT_CHANGED)
	// MonitorEventSendFlowPaused selects the paired DEALER/ROUTER completion-lane
	// PAUSED-transition event (core-byte-hwm-flow-control-plan.ko.md §6).
	MonitorEventSendFlowPaused MonitorEventMask = MonitorEventMask(C.ZLINK_SOCKET_MONITOR_EVENT_SEND_FLOW_PAUSED)
	// MonitorEventSendFlowResumed selects the paired completion-lane RUNNING-transition event.
	MonitorEventSendFlowResumed MonitorEventMask = MonitorEventMask(C.ZLINK_SOCKET_MONITOR_EVENT_SEND_FLOW_RESUMED)
	// MonitorEventFlowStateStale selects a rejected stale or duplicate flow-state frame event.
	MonitorEventFlowStateStale MonitorEventMask = MonitorEventMask(C.ZLINK_SOCKET_MONITOR_EVENT_FLOW_STATE_STALE)
	MonitorEventAll            MonitorEventMask = MonitorEventMask(C.ZLINK_SOCKET_MONITOR_EVENT_ALL)
)

type MonitorEventMask uint32
type MonitorSourceKind uint32
type MonitorEventType uint64

// MonitorOpenOption configures a socket monitor when it is opened.
type MonitorOpenOption interface {
	applyMonitorOpenOption(*monitorOpenConfig)
}

type monitorOpenConfig struct {
	events          MonitorEventMask
	hasEvents       bool
	monitorHwmBytes uint64
}

type monitorHwmBytesOption uint64

func (events MonitorEventMask) applyMonitorOpenOption(config *monitorOpenConfig) {
	config.events |= events
	config.hasEvents = true
}

func (value monitorHwmBytesOption) applyMonitorOpenOption(config *monitorOpenConfig) {
	config.monitorHwmBytes = uint64(value)
}

// MonitorHwmBytes sets the monitor queue HWM in bytes. Zero selects the Core
// default; a positive value is passed to Core unchanged. If supplied more than
// once, the last value wins.
func MonitorHwmBytes(value uint64) MonitorOpenOption {
	return monitorHwmBytesOption(value)
}

const (
	MonitorEventTypeConnected               MonitorEventType = MonitorEventType(MonitorEventConnected)
	MonitorEventTypeConnectDelayed          MonitorEventType = MonitorEventType(MonitorEventConnectDelayed)
	MonitorEventTypeConnectRetried          MonitorEventType = MonitorEventType(MonitorEventConnectRetried)
	MonitorEventTypeListening               MonitorEventType = MonitorEventType(MonitorEventListening)
	MonitorEventTypeBindFailed              MonitorEventType = MonitorEventType(MonitorEventBindFailed)
	MonitorEventTypeAccepted                MonitorEventType = MonitorEventType(MonitorEventAccepted)
	MonitorEventTypeAcceptFailed            MonitorEventType = MonitorEventType(MonitorEventAcceptFailed)
	MonitorEventTypeClosed                  MonitorEventType = MonitorEventType(MonitorEventClosed)
	MonitorEventTypeCloseFailed             MonitorEventType = MonitorEventType(MonitorEventCloseFailed)
	MonitorEventTypeDisconnected            MonitorEventType = MonitorEventType(MonitorEventDisconnected)
	MonitorEventTypeMonitorStopped          MonitorEventType = MonitorEventType(MonitorEventMonitorStopped)
	MonitorEventTypeHandshakeFailedNoDetail MonitorEventType = MonitorEventType(MonitorEventHandshakeFailedNoDetail)
	MonitorEventTypeConnectionReady         MonitorEventType = MonitorEventType(MonitorEventConnectionReady)
	MonitorEventTypeHandshakeFailedProtocol MonitorEventType = MonitorEventType(MonitorEventHandshakeFailedProtocol)
	MonitorEventTypeHandshakeFailedAuth     MonitorEventType = MonitorEventType(MonitorEventHandshakeFailedAuth)
	MonitorEventTypePeerWeightChanged       MonitorEventType = MonitorEventType(MonitorEventPeerWeightChanged)
	MonitorEventTypeSendFlowPaused          MonitorEventType = MonitorEventType(MonitorEventSendFlowPaused)
	MonitorEventTypeSendFlowResumed         MonitorEventType = MonitorEventType(MonitorEventSendFlowResumed)
	MonitorEventTypeFlowStateStale          MonitorEventType = MonitorEventType(MonitorEventFlowStateStale)
	MonitorEventTypeAll                     MonitorEventType = MonitorEventType(MonitorEventAll)
)

// MonitorEventFlag is a bitmask of event-specific detail flags carried in
// zlink_monitor_event_t.flags (core-byte-hwm-flow-control-plan.ko.md §6).
type MonitorEventFlag uint32

const (
	// MonitorEventFlagConnectionReadyEdge is set on MonitorEventTypeConnectionReady
	// when the event changes a connection from not-ready to ready.
	MonitorEventFlagConnectionReadyEdge MonitorEventFlag = MonitorEventFlag(C.ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE)
	// MonitorEventFlagSendFlowWritable is set on MonitorEventTypeSendFlowResumed when
	// clearing the remote pause left the pipe actually writable. Value carries the flow epoch.
	MonitorEventFlagSendFlowWritable MonitorEventFlag = MonitorEventFlag(C.ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE)
	// MonitorEventFlagFlowStateStaleGeneration is set on MonitorEventTypeFlowStateStale
	// when the frame named a different connection generation. Value carries the
	// received generation; TransportPairGeneration carries the current one.
	MonitorEventFlagFlowStateStaleGeneration MonitorEventFlag = MonitorEventFlag(C.ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_GENERATION)
	// MonitorEventFlagFlowStateStaleEpoch is set on MonitorEventTypeFlowStateStale when
	// the epoch did not advance inside the current generation. Value carries the
	// received epoch.
	MonitorEventFlagFlowStateStaleEpoch MonitorEventFlag = MonitorEventFlag(C.ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH)
)

// MonitorTransportLane identifies the transport lane associated with a monitor event.
type MonitorTransportLane uint32

const (
	MonitorTransportLaneApplication MonitorTransportLane = MonitorTransportLane(C.ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION)
	MonitorTransportLaneCompletion  MonitorTransportLane = MonitorTransportLane(C.ZLINK_MONITOR_TRANSPORT_LANE_COMPLETION)
)

const (
	MonitorSourceSocket MonitorSourceKind = MonitorSourceKind(C.ZLINK_MONITOR_SOURCE_SOCKET)
)

type MonitorEvent struct {
	Event      MonitorEventType
	Value      uint64
	RoutingID  RoutingID
	LocalAddr  string
	RemoteAddr string
	// ConnectionID is the process-local identity of the physical transport attempt.
	ConnectionID uint64
	// TransportPairID is non-zero for a paired Application/Completion transport.
	TransportPairID uint64
	// TransportPairGeneration is the generation of the paired transport. Zero for
	// an unpaired transport.
	TransportPairGeneration uint64
	// TransportLane identifies which lane of a paired transport this event describes.
	TransportLane MonitorTransportLane
	// Flags carries event-specific detail bits; see MonitorEventFlag.
	Flags MonitorEventFlag
}

func (e *MonitorEvent) HasRoutingID() bool {
	return e != nil && monitorHasRoutingID(e.RoutingID)
}

func (e *MonitorEvent) IsConnected() bool {
	return e != nil && e.Event&MonitorEventTypeConnected != 0
}

func (e *MonitorEvent) IsDisconnected() bool {
	return e != nil && e.Event&MonitorEventTypeDisconnected != 0
}

func (e *MonitorEvent) IsListening() bool {
	return e != nil && e.Event&MonitorEventTypeListening != 0
}

func (e *MonitorEvent) IsAccepted() bool {
	return e != nil && e.Event&MonitorEventTypeAccepted != 0
}

func (e *MonitorEvent) IsConnectionReady() bool {
	return e != nil && e.Event&MonitorEventTypeConnectionReady != 0
}

func (e *MonitorEvent) IsSendFlowPaused() bool {
	return e != nil && e.Event&MonitorEventTypeSendFlowPaused != 0
}

func (e *MonitorEvent) IsSendFlowResumed() bool {
	return e != nil && e.Event&MonitorEventTypeSendFlowResumed != 0
}

func (e *MonitorEvent) IsFlowStateStale() bool {
	return e != nil && e.Event&MonitorEventTypeFlowStateStale != 0
}

func (e *MonitorEvent) HasFlag(flag MonitorEventFlag) bool {
	return e != nil && e.Flags&flag != 0
}

type MonitorStatus struct {
	ABIVersion                       uint32
	StructSize                       uint32
	SourceKind                       MonitorSourceKind
	StateFlags                       uint32
	DetailFlags                      uint32
	SndPendingMsgs                   uint64
	RcvPendingMsgs                   uint64
	SndPendingBytes                  uint64
	RcvPendingBytes                  uint64
	AutoHwmEnabled                   bool
	AutoHwmProfile                   uint32
	AutoHwmRole                      uint32
	AutoHwmPolicyClass               uint32
	AutoHwmPlannedSndHwmBytes        uint64
	AutoHwmPlannedRcvHwmBytes        uint64
	AutoHwmAppliedSndHwmBytes        uint64
	AutoHwmAppliedRcvHwmBytes        uint64
	AutoHwmEffectiveSndBuf           int32
	AutoHwmEffectiveRcvBuf           int32
	AutoHwmLastRecalcMs              uint64
	AutoHwmLastRecalcReason          AutoHwmRecalcReason
	AutoHwmSendBlockedRatioPPM       uint32
	AutoHwmDeferredSndHwmBytes       uint64
	AutoHwmDeferredRcvHwmBytes       uint64
	AutoHwmDeferredSndHwmValid       bool
	AutoHwmDeferredRcvHwmValid       bool
	SndBytesInFlight                 uint64
	RcvBytesInFlight                 uint64
	MinimumCoreMessageChargeBytes    uint64
	OversizeMessageAdmissionCount    uint64
	OversizeMessageAdmissionMaxBytes uint64
	// Paired DEALER/ROUTER completion-lane receive-flow observation
	// (core-byte-hwm-flow-control-plan.ko.md §6). Present since ABI 4; zero on
	// socket types with no completion lane.
	FlowPausedConnections  uint64
	FlowPauseAppliedTotal  uint64
	FlowResumeAppliedTotal uint64
	FlowStateStaleTotal    uint64
	FlowPauseDurationMs    uint64
}

func (s *MonitorStatus) IsReady() bool {
	return s != nil && s.StateFlags&uint32(C.ZLINK_MONITOR_STATE_READY) != 0
}

// IsFlowStateDetailPopulated reports whether FlowPausedConnections and the
// other flow_* fields are populated (ABI 4+, paired DEALER/ROUTER sockets
// only). Mirrors ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE in
// core/include/zlink_enum.h.
func (s *MonitorStatus) IsFlowStateDetailPopulated() bool {
	return s != nil && s.DetailFlags&uint32(C.ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE) != 0
}

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
	handle     atomic.Pointer[byte]
	callbackMu sync.Mutex
	callback   cgo.Handle
}

func resolveMonitorOpenOptions(options []MonitorOpenOption) (monitorOpenConfig, error) {
	config := monitorOpenConfig{}
	for _, option := range options {
		if option == nil {
			return monitorOpenConfig{}, &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
		}
		option.applyMonitorOpenOption(&config)
	}
	if !config.hasEvents {
		config.events = MonitorEventAll
	}
	return config, nil
}

func OpenSocketMonitor(socket SocketTarget, options ...MonitorOpenOption) (*SocketMonitor, error) {
	if socket == nil {
		return nil, &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	config, err := resolveMonitorOpenOptions(options)
	if err != nil {
		return nil, err
	}
	nativeOptions := C.zlink_socket_monitor_open_options_t{
		events:            C.zlink_socket_monitor_event_mask_t(config.events),
		monitor_hwm_bytes: C.uint64_t(config.monitorHwmBytes),
	}
	handle := C.zlink_socket_monitor_open(socket.raw(), &nativeOptions)
	if handle == nil {
		return nil, configErrorFromErrno(currentErrno())
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
//
// A blocking recv interrupted by a signal (RecvInternalError with an EINTR
// errno) is a spurious wakeup, not a caller-visible failure: Poller.Wait
// treats the same core signal identically (see poller_timer.go), so Recv
// retries internally instead of surfacing EINTR to the caller.
func (m *SocketMonitor) Recv(flags RecvFlags) (*MonitorEvent, error) {
	handle := m.raw()
	if handle == nil {
		return nil, &RecvError{Result: RecvInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	var raw C.zlink_socket_monitor_event_t
	for {
		err := recvErrorFromResult(C.zlink_socket_monitor_recv(handle, &raw, C.zlink_recv_flags_t(flags)))
		if err == nil {
			return monitorEventFromC(raw), nil
		}
		var recvErr *RecvError
		if errors.As(err, &recvErr) && recvErr.Result == RecvInternalError && recvErr.internalErrno() == int(C.EINTR) {
			continue
		}
		return nil, err
	}
}

func (m *SocketMonitor) Status() (*MonitorStatus, error) {
	handle := m.raw()
	if handle == nil {
		return nil, &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	var raw C.zlink_monitor_status_t
	if err := configErrorFromResult(C.zlink_monitor_status(handle, &raw)); err != nil {
		return nil, err
	}
	snapshot := monitorStatusFromC(raw)
	return &snapshot, nil
}

func (m *SocketMonitor) OnEvent(handler func(*MonitorEvent)) error {
	if handler == nil {
		return &HandlerError{Result: HandlerInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	if m == nil {
		return &HandlerError{Result: HandlerInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	m.callbackMu.Lock()
	defer m.callbackMu.Unlock()
	handlePtr := m.raw()
	if handlePtr == nil {
		return &HandlerError{Result: HandlerInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	state := newMonitorCallbackState(handler)
	handle := cgo.NewHandle(state)
	if err := handlerErrorFromResult(C.zlink_socket_monitor_handler_go_local(handlePtr, C.uintptr_t(handle))); err != nil {
		state.close()
		handle.Delete()
		return err
	}
	if m.callback != 0 {
		releaseCallbackHandle(m.callback)
	}
	m.callback = handle
	return nil
}

func (m *SocketMonitor) Close() error {
	if m == nil {
		return nil
	}
	m.callbackMu.Lock()
	handlePtr := m.raw()
	if handlePtr == nil {
		m.callbackMu.Unlock()
		return nil
	}
	handle := handlePtr
	if err := closeErrorFromResult(C.zlink_monitor_close(&handle)); err != nil {
		m.callbackMu.Unlock()
		return err
	}
	m.handle.Store(nil)
	callback := m.callback
	m.callback = 0
	m.callbackMu.Unlock()
	if callback != 0 {
		releaseCallbackHandle(callback)
	}
	return nil
}

func monitorEventFromC(raw C.zlink_socket_monitor_event_t) *MonitorEvent {
	return &MonitorEvent{
		Event:                   MonitorEventType(raw.event),
		Value:                   uint64(raw.value),
		RoutingID:               routingIDFromC(raw.routing_id),
		LocalAddr:               C.GoString(&raw.local_addr[0]),
		RemoteAddr:              C.GoString(&raw.remote_addr[0]),
		ConnectionID:            uint64(raw.connection_id),
		TransportPairID:         uint64(raw.transport_pair_id),
		TransportPairGeneration: uint64(raw.transport_pair_generation),
		TransportLane:           MonitorTransportLane(raw.transport_lane),
		Flags:                   MonitorEventFlag(raw.flags),
	}
}

// monitorEventFromRawFieldsForTest exercises monitorEventFromC's field-by-field
// conversion from plain Go values, so a non-cgo test file in this package can
// verify the 64-bit round-trip without itself importing "C" — this package's
// cgo //export trampolines (goZlinkMonitorTrampoline, etc.) make a second
// cgo-using compilation unit for its own _test.go files unsupported by the Go
// toolchain ("use of cgo in test not supported").
func monitorEventFromRawFieldsForTest(
	eventType MonitorEventType,
	value, connectionID, transportPairID, transportPairGeneration uint64,
	transportLane MonitorTransportLane,
	flags MonitorEventFlag,
) *MonitorEvent {
	var raw C.zlink_socket_monitor_event_t
	raw.event = C.uint64_t(eventType)
	raw.value = C.uint64_t(value)
	raw.connection_id = C.uint64_t(connectionID)
	raw.transport_pair_id = C.uint64_t(transportPairID)
	raw.transport_pair_generation = C.uint64_t(transportPairGeneration)
	raw.transport_lane = C.uint32_t(transportLane)
	raw.flags = C.uint32_t(flags)
	return monitorEventFromC(raw)
}

//export goZlinkMonitorTrampoline
func goZlinkMonitorTrampoline(event *C.zlink_monitor_event_t, userdata C.uintptr_t) {
	state, ok := safeHandleAs[*monitorCallbackState](userdata)
	if !ok {
		return
	}
	payload := monitorEventFromC(*event)
	state.dispatcher.enqueue(&callbackTask{
		label: "socket-monitor",
		invoke: func() {
			state.handler(payload)
		},
	})
}
