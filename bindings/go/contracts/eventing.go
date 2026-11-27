// SPDX-License-Identifier: MPL-2.0

package contracts

// PollEventFlag is a bitmask of poll readiness event flags.
type PollEventFlag int16

const (
	PollIn         PollEventFlag = 1
	PollOut        PollEventFlag = 2
	PollErr        PollEventFlag = 4
	PollPri        PollEventFlag = 8
	PollCompletion PollEventFlag = 32
	// PollRoute reports that a ROUTER's selected route changed; read RoutesSnapshot().
	PollRoute PollEventFlag = 64
	// PollConnect reports that a raw socket's connection attempts changed; read ConnectionAttemptsSnapshot().
	PollConnect PollEventFlag = 128
)

// PollSourceKind identifies the kind of source in a PollEvent.
type PollSourceKind int32

const (
	PollSourceSocket PollSourceKind = 1
	PollSourceFD     PollSourceKind = 2
	PollSourceTimer  PollSourceKind = 3
)
const (
	MonitorEventConnected               MonitorEventMask = MonitorEventMask(1 << 0)
	MonitorEventConnectDelayed          MonitorEventMask = MonitorEventMask(1 << 1)
	MonitorEventConnectRetried          MonitorEventMask = MonitorEventMask(1 << 2)
	MonitorEventListening               MonitorEventMask = MonitorEventMask(1 << 3)
	MonitorEventBindFailed              MonitorEventMask = MonitorEventMask(1 << 4)
	MonitorEventAccepted                MonitorEventMask = MonitorEventMask(1 << 5)
	MonitorEventAcceptFailed            MonitorEventMask = MonitorEventMask(1 << 6)
	MonitorEventClosed                  MonitorEventMask = MonitorEventMask(1 << 7)
	MonitorEventCloseFailed             MonitorEventMask = MonitorEventMask(1 << 8)
	MonitorEventDisconnected            MonitorEventMask = MonitorEventMask(1 << 9)
	MonitorEventMonitorStopped          MonitorEventMask = MonitorEventMask(1 << 10)
	MonitorEventHandshakeFailedNoDetail MonitorEventMask = MonitorEventMask(1 << 11)
	MonitorEventConnectionReady         MonitorEventMask = MonitorEventMask(1 << 12)
	MonitorEventHandshakeFailedProtocol MonitorEventMask = MonitorEventMask(1 << 13)
	MonitorEventHandshakeFailedAuth     MonitorEventMask = MonitorEventMask(1 << 14)
	MonitorEventPeerWeightChanged       MonitorEventMask = MonitorEventMask(1 << 15)
	// MonitorEventSendFlowPaused selects the PAUSED-transition event for an
	// affected Application pipe (core-byte-hwm-flow-control-plan.ko.md §6).
	MonitorEventSendFlowPaused MonitorEventMask = MonitorEventMask(1 << 16)
	// MonitorEventSendFlowResumed selects the RUNNING-transition event for an affected Application pipe.
	MonitorEventSendFlowResumed MonitorEventMask = MonitorEventMask(1 << 17)
	// MonitorEventFlowStateStale selects a rejected stale or duplicate flow-state frame event.
	MonitorEventFlowStateStale MonitorEventMask = MonitorEventMask(1 << 18)
	MonitorEventAll            MonitorEventMask = MonitorEventMask(0x7ffff)
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

// MonitorHwmBytes selects the monitor queue HWM in bytes.
func MonitorHwmBytes(value uint64) MonitorOpenOption { return monitorHwmBytesOption(value) }

// ResolveMonitorOpenOptions resolves the monitor mask and HWM. A false result
// reports a nil option; the caller maps it to the binding's config error.
func ResolveMonitorOpenOptions(options []MonitorOpenOption) (MonitorEventMask, uint64, bool) {
	config := monitorOpenConfig{}
	for _, option := range options {
		if option == nil {
			return 0, 0, false
		}
		option.applyMonitorOpenOption(&config)
	}
	if !config.hasEvents {
		config.events = MonitorEventAll
	}
	return config.events, config.monitorHwmBytes, true
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
	MonitorEventFlagConnectionReadyEdge MonitorEventFlag = MonitorEventFlag(1 << 0)
	// MonitorEventFlagSendFlowWritable is set on MonitorEventTypeSendFlowResumed when
	// clearing the remote pause left the pipe actually writable. Value carries the flow epoch.
	MonitorEventFlagSendFlowWritable MonitorEventFlag = MonitorEventFlag(1 << 1)
	// MonitorEventFlagFlowStateStaleEpoch is set on MonitorEventTypeFlowStateStale when
	// the epoch did not advance inside the current generation. Value carries the
	// received epoch.
	MonitorEventFlagFlowStateStaleEpoch MonitorEventFlag = MonitorEventFlag(1 << 3)
)

// MonitorTransportLane identifies the transport lane associated with a monitor event.
type MonitorTransportLane uint32

const (
	MonitorTransportLaneApplication MonitorTransportLane = MonitorTransportLane(0)
	MonitorTransportLaneCompletion  MonitorTransportLane = MonitorTransportLane(1)
)

const (
	MonitorSourceSocket MonitorSourceKind = MonitorSourceKind(1)
)

type MonitorEvent struct {
	Event      MonitorEventType
	Value      uint64
	RoutingID  RoutingID
	LocalAddr  string
	RemoteAddr string
	// ConnectionID is the process-local identity of the physical transport attempt.
	ConnectionID uint64
	// TransportLane identifies which lane of a paired transport this event describes.
	TransportLane MonitorTransportLane
	// Flags carries event-specific detail bits; see MonitorEventFlag.
	Flags MonitorEventFlag
}

func (e *MonitorEvent) HasRoutingID() bool {
	return e != nil && e.RoutingID.Size() > 0
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
	// DEALER/ROUTER receive-flow observation for affected Application pipes.
	// Control uses the Application connection for count-1 peers and the
	// Completion connection for count-2 ROUTER-ROUTER peers. Present since ABI 4;
	// zero on socket types other than DEALER/ROUTER.
	FlowPausedConnections  uint64
	FlowPauseAppliedTotal  uint64
	FlowResumeAppliedTotal uint64
	FlowStateStaleTotal    uint64
	FlowPauseDurationMs    uint64
}

// MonitorStateFlags is the typed view of MonitorStatus.StateFlags. Bit values
// mirror zlink_monitor_state_flag_e in core/include/zlink_enum.h.
type MonitorStateFlags uint32

const (
	MonitorStateReady      MonitorStateFlags = 1 << 0
	MonitorStateBoundReady MonitorStateFlags = 1 << 1
	MonitorStateClosed     MonitorStateFlags = 1 << 3
)

// Has reports whether every bit of flag is set.
func (f MonitorStateFlags) Has(flag MonitorStateFlags) bool { return f&flag == flag }

// MonitorStatusDetailFlags is the typed view of MonitorStatus.DetailFlags. Bit
// values mirror zlink_monitor_status_detail_flag_e in core/include/zlink_enum.h.
type MonitorStatusDetailFlags uint32

const (
	MonitorDetailSndPendingMsgs MonitorStatusDetailFlags = 1 << 1
	MonitorDetailRcvPendingMsgs MonitorStatusDetailFlags = 1 << 2
	MonitorDetailAutoHwmBudget  MonitorStatusDetailFlags = 1 << 3
	MonitorDetailAutoHwmBuffers MonitorStatusDetailFlags = 1 << 4
	MonitorDetailFlowState      MonitorStatusDetailFlags = 1 << 5
)

// Has reports whether every bit of flag is set.
func (f MonitorStatusDetailFlags) Has(flag MonitorStatusDetailFlags) bool { return f&flag == flag }

// State returns StateFlags as typed flags.
func (s *MonitorStatus) State() MonitorStateFlags {
	if s == nil {
		return 0
	}
	return MonitorStateFlags(s.StateFlags)
}

// Detail returns DetailFlags as typed flags.
func (s *MonitorStatus) Detail() MonitorStatusDetailFlags {
	if s == nil {
		return 0
	}
	return MonitorStatusDetailFlags(s.DetailFlags)
}

func (s *MonitorStatus) IsReady() bool {
	return s.State().Has(MonitorStateReady)
}

// IsFlowStateDetailPopulated reports whether FlowPausedConnections and the
// other flow_* fields are populated (ABI 4+, paired DEALER/ROUTER sockets
// only). Mirrors ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE in
// core/include/zlink_enum.h.
func (s *MonitorStatus) IsFlowStateDetailPopulated() bool {
	return s.Detail().Has(MonitorDetailFlowState)
}
