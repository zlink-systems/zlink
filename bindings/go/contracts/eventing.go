// SPDX-License-Identifier: MPL-2.0

package contracts

import impl "zlink.systems/zlink/v11/internal/native"

type (
	// MonitorEventMask is a bitmask selecting which socket monitor events to subscribe to.
	MonitorEventMask = impl.MonitorEventMask
	// MonitorSourceKind identifies what a monitored source is.
	MonitorSourceKind = impl.MonitorSourceKind
	// MonitorEventType is the kind of a delivered socket monitor event.
	MonitorEventType = impl.MonitorEventType
	// MonitorEvent is a single socket connection-lifecycle event reported by a monitor.
	MonitorEvent = impl.MonitorEvent
	// MonitorStatus is a snapshot of a monitored socket's state and auto-high-water-mark telemetry.
	MonitorStatus = impl.MonitorStatus
	// SocketMonitor observes a socket's connection lifecycle events and current status.
	SocketMonitor = impl.SocketMonitor
	// PollEventFlag is a readiness condition a poll source can be watched for or report.
	PollEventFlag = impl.PollEventFlag
	// PollSourceKind is whether a poll event came from a socket, file descriptor, or timer.
	PollSourceKind = impl.PollSourceKind
	// PollItem is a raw poll descriptor: a file descriptor and its watched/fired events.
	PollItem = impl.PollItem
	// PollEvent is one ready source reported by a poller wait.
	PollEvent = impl.PollEvent
	// Poller multiplexes sockets, file descriptors, and timers, reporting which become ready.
	Poller = impl.Poller
	// Timer fires on an interval and can be polled or awaited.
	Timer = impl.Timer
)

const (
	// MonitorEventConnected selects a connection-established event.
	MonitorEventConnected = impl.MonitorEventConnected
	// MonitorEventConnectDelayed selects an asynchronous connect-in-progress event.
	MonitorEventConnectDelayed = impl.MonitorEventConnectDelayed
	// MonitorEventConnectRetried selects a connect-retry event.
	MonitorEventConnectRetried = impl.MonitorEventConnectRetried
	// MonitorEventListening selects a listening-state event.
	MonitorEventListening = impl.MonitorEventListening
	// MonitorEventBindFailed selects a bind-failed event.
	MonitorEventBindFailed = impl.MonitorEventBindFailed
	// MonitorEventAccepted selects an accepted-connection event.
	MonitorEventAccepted = impl.MonitorEventAccepted
	// MonitorEventAcceptFailed selects an accept-failed event.
	MonitorEventAcceptFailed = impl.MonitorEventAcceptFailed
	// MonitorEventClosed selects a closed-connection event.
	MonitorEventClosed = impl.MonitorEventClosed
	// MonitorEventCloseFailed selects a close-failed event.
	MonitorEventCloseFailed = impl.MonitorEventCloseFailed
	// MonitorEventDisconnected selects a peer-disconnected event.
	MonitorEventDisconnected = impl.MonitorEventDisconnected
	// MonitorEventMonitorStopped selects a monitor-stopped event.
	MonitorEventMonitorStopped = impl.MonitorEventMonitorStopped
	// MonitorEventHandshakeFailedNoDetail selects a handshake failure without detail.
	MonitorEventHandshakeFailedNoDetail = impl.MonitorEventHandshakeFailedNoDetail
	// MonitorEventHandshakeFailedProtocol selects a protocol handshake failure.
	MonitorEventHandshakeFailedProtocol = impl.MonitorEventHandshakeFailedProtocol
	// MonitorEventHandshakeFailedAuth selects an authentication handshake failure.
	MonitorEventHandshakeFailedAuth = impl.MonitorEventHandshakeFailedAuth
	// MonitorEventAll subscribes the monitor to every connection lifecycle event.
	MonitorEventAll = impl.MonitorEventAll
	// MonitorEventConnectionReady fires when a connection completes its handshake and is ready for traffic.
	MonitorEventConnectionReady = impl.MonitorEventConnectionReady
	// MonitorEventPeerWeightChanged fires when a peer's load-balancing weight changes.
	MonitorEventPeerWeightChanged = impl.MonitorEventPeerWeightChanged
	// MonitorEventTypeConnected is the delivered value for a connection-established event.
	MonitorEventTypeConnected = impl.MonitorEventTypeConnected
	// MonitorEventTypeConnectDelayed is the delivered value for a connect-in-progress event.
	MonitorEventTypeConnectDelayed = impl.MonitorEventTypeConnectDelayed
	// MonitorEventTypeConnectRetried is the delivered value for a connect-retry event.
	MonitorEventTypeConnectRetried = impl.MonitorEventTypeConnectRetried
	// MonitorEventTypeListening is the delivered value for a listening-state event.
	MonitorEventTypeListening = impl.MonitorEventTypeListening
	// MonitorEventTypeBindFailed is the delivered value for a bind-failed event.
	MonitorEventTypeBindFailed = impl.MonitorEventTypeBindFailed
	// MonitorEventTypeAccepted is the delivered value for an accepted-connection event.
	MonitorEventTypeAccepted = impl.MonitorEventTypeAccepted
	// MonitorEventTypeAcceptFailed is the delivered value for an accept-failed event.
	MonitorEventTypeAcceptFailed = impl.MonitorEventTypeAcceptFailed
	// MonitorEventTypeClosed is the delivered value for a closed-connection event.
	MonitorEventTypeClosed = impl.MonitorEventTypeClosed
	// MonitorEventTypeCloseFailed is the delivered value for a close-failed event.
	MonitorEventTypeCloseFailed = impl.MonitorEventTypeCloseFailed
	// MonitorEventTypeDisconnected is the delivered value for a peer-disconnected event.
	MonitorEventTypeDisconnected = impl.MonitorEventTypeDisconnected
	// MonitorEventTypeMonitorStopped is the delivered value for a monitor-stopped event.
	MonitorEventTypeMonitorStopped = impl.MonitorEventTypeMonitorStopped
	// MonitorEventTypeHandshakeFailedNoDetail is the delivered value for an undescribed handshake failure.
	MonitorEventTypeHandshakeFailedNoDetail = impl.MonitorEventTypeHandshakeFailedNoDetail
	// MonitorEventTypeConnectionReady is the delivered value for a ready connection.
	MonitorEventTypeConnectionReady = impl.MonitorEventTypeConnectionReady
	// MonitorEventTypeHandshakeFailedProtocol is the delivered value for a protocol handshake failure.
	MonitorEventTypeHandshakeFailedProtocol = impl.MonitorEventTypeHandshakeFailedProtocol
	// MonitorEventTypeHandshakeFailedAuth is the delivered value for an authentication handshake failure.
	MonitorEventTypeHandshakeFailedAuth = impl.MonitorEventTypeHandshakeFailedAuth
	// MonitorEventTypePeerWeightChanged is the delivered value for a peer weight change.
	MonitorEventTypePeerWeightChanged = impl.MonitorEventTypePeerWeightChanged
	// MonitorEventTypeAll is the mask value representing every monitor event.
	MonitorEventTypeAll = impl.MonitorEventTypeAll
	// MonitorSourceSocket identifies a plain socket as the monitored source.
	MonitorSourceSocket = impl.MonitorSourceSocket
	// PollIn reports that a receive will not block.
	PollIn = impl.PollIn
	// PollOut reports that a send will not block.
	PollOut = impl.PollOut
	// PollErr reports an error condition on the source.
	PollErr = impl.PollErr
	// PollPri reports that priority or out-of-band data is available.
	PollPri = impl.PollPri
	// PollCompletion reports that an asynchronous operation has completed.
	PollCompletion = impl.PollCompletion
	// PollSourceSocket identifies a socket as the source of a poll event.
	PollSourceSocket = impl.PollSourceSocket
	// PollSourceFD identifies a raw file descriptor as the source of a poll event.
	PollSourceFD = impl.PollSourceFD
	// PollSourceTimer identifies a timer as the source of a poll event.
	PollSourceTimer = impl.PollSourceTimer
)

var (
	// OpenSocketMonitor opens a monitor on a socket for the selected events; the caller owns it.
	OpenSocketMonitor = impl.OpenSocketMonitor
	// NewTimer creates a standalone timer; the caller owns it.
	NewTimer = impl.NewTimer
	// NewPoller creates an empty poller; the caller owns it.
	NewPoller = impl.NewPoller
	// Poll waits for events across several sockets or monitors at once.
	Poll = impl.Poll
)
