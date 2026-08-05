// SPDX-License-Identifier: MPL-2.0

// Package zlink projects the public Go binding contract at the module root.
package zlink

import impl "zlink.systems/zlink/v11/contracts"

type (
	SendOp                  = impl.SendOp
	SendSubmitOp            = impl.SendSubmitOp
	RequestOp               = impl.RequestOp
	RequestSubmitOp         = impl.RequestSubmitOp
	RequestCallbackSubmitOp = impl.RequestCallbackSubmitOp
	ReplyOp                 = impl.ReplyOp
	ReplySubmitOp           = impl.ReplySubmitOp

	MonitorEventMask  = impl.MonitorEventMask
	MonitorSourceKind = impl.MonitorSourceKind
	MonitorEventType  = impl.MonitorEventType
	MonitorEvent      = impl.MonitorEvent
	MonitorStatus     = impl.MonitorStatus
	SocketMonitor     = impl.SocketMonitor

	RoutingID              = impl.RoutingID
	Message                = impl.Message
	RequestReplyCallback   = impl.RequestReplyCallback
	RequestReplyCompletion = impl.RequestReplyCompletion
	Received               = impl.Received
	TopicMessage           = impl.TopicMessage
	SubscriptionEvent      = impl.SubscriptionEvent

	Version             = impl.Version
	Context             = impl.Context
	ContextOptions      = impl.ContextOptions
	AutoHwmProfile      = impl.AutoHwmProfile
	AutoHwmRecalcReason = impl.AutoHwmRecalcReason
	SocketTarget        = impl.SocketTarget
	Stopwatch           = impl.Stopwatch
	AtomicCounter       = impl.AtomicCounter
	Thread              = impl.Thread

	SocketType          = impl.SocketType
	RidDuplicatePolicy  = impl.RidDuplicatePolicy
	SubmitRetryMode     = impl.SubmitRetryMode
	CommonSocketOptions = impl.CommonSocketOptions
	PubSocketOptions    = impl.PubSocketOptions
	PairSocket          = impl.PairSocket
	PubSocket           = impl.PubSocket
	SubSocket           = impl.SubSocket
	DealerSocket        = impl.DealerSocket
	RouterSocket        = impl.RouterSocket
	XPubSocket          = impl.XPubSocket
	XSubSocket          = impl.XSubSocket
	StreamSocket        = impl.StreamSocket
	SendFlags           = impl.SendFlags
	RecvFlags           = impl.RecvFlags

	PollEventFlag  = impl.PollEventFlag
	PollSourceKind = impl.PollSourceKind
	PollItem       = impl.PollItem
	PollEvent      = impl.PollEvent
	Poller         = impl.Poller
	Timer          = impl.Timer

	ZlinkError    = impl.ZlinkError
	SubmitError   = impl.SubmitError
	RequestError  = impl.RequestError
	RecvError     = impl.RecvError
	HandlerError  = impl.HandlerError
	CloseError    = impl.CloseError
	BindError     = impl.BindError
	ConnectError  = impl.ConnectError
	ConfigError   = impl.ConfigError
	SubmitResult  = impl.SubmitResult
	RequestResult = impl.RequestResult
	RecvResult    = impl.RecvResult
	HandlerResult = impl.HandlerResult
	CloseResult   = impl.CloseResult
	BindResult    = impl.BindResult
	ConnectResult = impl.ConnectResult
	ConfigResult  = impl.ConfigResult
)

const (
	SocketTypeAny    = impl.SocketTypeAny
	SocketTypePair   = impl.SocketTypePair
	SocketTypePub    = impl.SocketTypePub
	SocketTypeSub    = impl.SocketTypeSub
	SocketTypeDealer = impl.SocketTypeDealer
	SocketTypeRouter = impl.SocketTypeRouter
	SocketTypeXPub   = impl.SocketTypeXPub
	SocketTypeXSub   = impl.SocketTypeXSub
	SocketTypeStream = impl.SocketTypeStream

	MonitorEventConnected               = impl.MonitorEventConnected
	MonitorEventConnectDelayed          = impl.MonitorEventConnectDelayed
	MonitorEventConnectRetried          = impl.MonitorEventConnectRetried
	MonitorEventListening               = impl.MonitorEventListening
	MonitorEventBindFailed              = impl.MonitorEventBindFailed
	MonitorEventAccepted                = impl.MonitorEventAccepted
	MonitorEventAcceptFailed            = impl.MonitorEventAcceptFailed
	MonitorEventClosed                  = impl.MonitorEventClosed
	MonitorEventCloseFailed             = impl.MonitorEventCloseFailed
	MonitorEventDisconnected            = impl.MonitorEventDisconnected
	MonitorEventMonitorStopped          = impl.MonitorEventMonitorStopped
	MonitorEventHandshakeFailedNoDetail = impl.MonitorEventHandshakeFailedNoDetail
	MonitorEventConnectionReady         = impl.MonitorEventConnectionReady
	MonitorEventHandshakeFailedProtocol = impl.MonitorEventHandshakeFailedProtocol
	MonitorEventHandshakeFailedAuth     = impl.MonitorEventHandshakeFailedAuth
	MonitorEventPeerWeightChanged       = impl.MonitorEventPeerWeightChanged
	MonitorEventAll                     = impl.MonitorEventAll

	MonitorEventTypeConnected               = impl.MonitorEventTypeConnected
	MonitorEventTypeConnectDelayed          = impl.MonitorEventTypeConnectDelayed
	MonitorEventTypeConnectRetried          = impl.MonitorEventTypeConnectRetried
	MonitorEventTypeListening               = impl.MonitorEventTypeListening
	MonitorEventTypeBindFailed              = impl.MonitorEventTypeBindFailed
	MonitorEventTypeAccepted                = impl.MonitorEventTypeAccepted
	MonitorEventTypeAcceptFailed            = impl.MonitorEventTypeAcceptFailed
	MonitorEventTypeClosed                  = impl.MonitorEventTypeClosed
	MonitorEventTypeCloseFailed             = impl.MonitorEventTypeCloseFailed
	MonitorEventTypeDisconnected            = impl.MonitorEventTypeDisconnected
	MonitorEventTypeMonitorStopped          = impl.MonitorEventTypeMonitorStopped
	MonitorEventTypeHandshakeFailedNoDetail = impl.MonitorEventTypeHandshakeFailedNoDetail
	MonitorEventTypeConnectionReady         = impl.MonitorEventTypeConnectionReady
	MonitorEventTypeHandshakeFailedProtocol = impl.MonitorEventTypeHandshakeFailedProtocol
	MonitorEventTypeHandshakeFailedAuth     = impl.MonitorEventTypeHandshakeFailedAuth
	MonitorEventTypePeerWeightChanged       = impl.MonitorEventTypePeerWeightChanged
	MonitorEventTypeAll                     = impl.MonitorEventTypeAll
	MonitorSourceSocket                     = impl.MonitorSourceSocket

	AutoHwmProfileCompact             = impl.AutoHwmProfileCompact
	AutoHwmProfileLowLatency          = impl.AutoHwmProfileLowLatency
	AutoHwmProfileBalanced            = impl.AutoHwmProfileBalanced
	AutoHwmProfileThroughput          = impl.AutoHwmProfileThroughput
	AutoHwmRecalcReasonNone           = impl.AutoHwmRecalcReasonNone
	AutoHwmRecalcReasonInitial        = impl.AutoHwmRecalcReasonInitial
	AutoHwmRecalcReasonRoleChange     = impl.AutoHwmRecalcReasonRoleChange
	AutoHwmRecalcReasonPolicyToggle   = impl.AutoHwmRecalcReasonPolicyToggle
	AutoHwmRecalcReasonRefresh        = impl.AutoHwmRecalcReasonRefresh
	AutoHwmRecalcReasonDeferredShrink = impl.AutoHwmRecalcReasonDeferredShrink

	RidDuplicateReject      = impl.RidDuplicateReject
	RidDuplicateHandover    = impl.RidDuplicateHandover
	SubmitRetryOff          = impl.SubmitRetryOff
	SubmitRetryLocalFailure = impl.SubmitRetryLocalFailure
	SendFlagsNone           = impl.SendFlagsNone
	SendFlagsDontWait       = impl.SendFlagsDontWait
	RecvFlagsNone           = impl.RecvFlagsNone
	RecvFlagsDontWait       = impl.RecvFlagsDontWait
	PollIn                  = impl.PollIn
	PollOut                 = impl.PollOut
	PollErr                 = impl.PollErr
	PollPri                 = impl.PollPri
	PollCompletion          = impl.PollCompletion
	PollSourceSocket        = impl.PollSourceSocket
	PollSourceFD            = impl.PollSourceFD
	PollSourceTimer         = impl.PollSourceTimer

	SubmitOK              = impl.SubmitOK
	SubmitBackpressured   = impl.SubmitBackpressured
	SubmitNotConnected    = impl.SubmitNotConnected
	SubmitNotFound        = impl.SubmitNotFound
	SubmitTerminated      = impl.SubmitTerminated
	SubmitInvalidHandle   = impl.SubmitInvalidHandle
	SubmitInvalidArgument = impl.SubmitInvalidArgument
	SubmitNotSupported    = impl.SubmitNotSupported
	SubmitInvalidState    = impl.SubmitInvalidState
	SubmitThreadViolation = impl.SubmitThreadViolation
	SubmitOutOfMemory     = impl.SubmitOutOfMemory
	SubmitSeqExhausted    = impl.SubmitSeqExhausted
	SubmitInternalError   = impl.SubmitInternalError
	SubmitNotAdmitted     = impl.SubmitNotAdmitted

	RequestOK              = impl.RequestOK
	RequestTimedOut        = impl.RequestTimedOut
	RequestNotFound        = impl.RequestNotFound
	RequestTerminated      = impl.RequestTerminated
	RequestProtocolError   = impl.RequestProtocolError
	RequestInternalError   = impl.RequestInternalError
	RequestRejected        = impl.RequestRejected
	RequestConflict        = impl.RequestConflict
	RequestBusy            = impl.RequestBusy
	RequestNotConnected    = impl.RequestNotConnected
	RequestInvalidArgument = impl.RequestInvalidArgument
	RequestInvalidState    = impl.RequestInvalidState
	RequestNotSupported    = impl.RequestNotSupported
	RequestBackpressured   = impl.RequestBackpressured

	RecvOK             = impl.RecvOK
	RecvNoData         = impl.RecvNoData
	RecvBusy           = impl.RecvBusy
	RecvTerminated     = impl.RecvTerminated
	RecvInvalidHandle  = impl.RecvInvalidHandle
	RecvNotSupported   = impl.RecvNotSupported
	RecvInternalError  = impl.RecvInternalError
	RecvBufferTooSmall = impl.RecvBufferTooSmall
	RecvInvalidState   = impl.RecvInvalidState

	HandlerOK              = impl.HandlerOK
	HandlerInvalidArgument = impl.HandlerInvalidArgument
	HandlerBusy            = impl.HandlerBusy
	HandlerNotSupported    = impl.HandlerNotSupported
	HandlerDeadlock        = impl.HandlerDeadlock
	HandlerInvalidHandle   = impl.HandlerInvalidHandle
	HandlerInternalError   = impl.HandlerInternalError

	CloseOK            = impl.CloseOK
	CloseBusy          = impl.CloseBusy
	CloseShutdown      = impl.CloseShutdown
	CloseInvalidHandle = impl.CloseInvalidHandle
	CloseInternalError = impl.CloseInternalError

	BindOK              = impl.BindOK
	BindInvalidArgument = impl.BindInvalidArgument
	BindAddrInUse       = impl.BindAddrInUse
	BindNotSupported    = impl.BindNotSupported
	BindInvalidHandle   = impl.BindInvalidHandle
	BindInternalError   = impl.BindInternalError

	ConnectOK              = impl.ConnectOK
	ConnectInvalidArgument = impl.ConnectInvalidArgument
	ConnectNotSupported    = impl.ConnectNotSupported
	ConnectInvalidHandle   = impl.ConnectInvalidHandle
	ConnectInternalError   = impl.ConnectInternalError
	ConnectNotFound        = impl.ConnectNotFound
	ConnectConflict        = impl.ConnectConflict
	ConnectBusy            = impl.ConnectBusy
	ConnectAuthFailed      = impl.ConnectAuthFailed

	ConfigOK              = impl.ConfigOK
	ConfigInvalidHandle   = impl.ConfigInvalidHandle
	ConfigInvalidArgument = impl.ConfigInvalidArgument
	ConfigNotSupported    = impl.ConfigNotSupported
	ConfigInternalError   = impl.ConfigInternalError
	ConfigInvalidState    = impl.ConfigInvalidState
	ConfigNotFound        = impl.ConfigNotFound
	ConfigConflict        = impl.ConfigConflict
	ConfigBufferTooSmall  = impl.ConfigBufferTooSmall
	ConfigBusy            = impl.ConfigBusy
)

var (
	OpenSocketMonitor     = impl.OpenSocketMonitor
	NewRoutingID          = impl.NewRoutingID
	NewRoutingIDString    = impl.NewRoutingIDString
	NewRoutingIDUint32    = impl.NewRoutingIDUint32
	NewRoutingIDUUIDBytes = impl.NewRoutingIDUUIDBytes
	NewRoutingIDFromHex   = impl.NewRoutingIDFromHex
	NewMessage            = impl.NewMessage
	NewMessageString      = impl.NewMessageString
	NewMessageWithSize    = impl.NewMessageWithSize
	RuntimeVersion        = impl.RuntimeVersion
	NewContext            = impl.NewContext
	NewTimer              = impl.NewTimer
	NewPoller             = impl.NewPoller
	Poll                  = impl.Poll
	Has                   = impl.Has
	Proxy                 = impl.Proxy
	ProxySteerable        = impl.ProxySteerable
	Sleep                 = impl.Sleep
	MultipartClose        = impl.MultipartClose
	NewStopwatch          = impl.NewStopwatch
	NewAtomicCounter      = impl.NewAtomicCounter
	NewThread             = impl.NewThread
)
