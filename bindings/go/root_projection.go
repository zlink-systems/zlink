// SPDX-License-Identifier: MPL-2.0

// Package zlink projects the public Go binding contract at the module root.
package zlink

import (
	"zlink.systems/zlink/contracts"
	"zlink.systems/zlink/internal/native"
)

type (
	SendOp            = native.SendOp
	SendSubmitOp      = native.SendSubmitOp
	SendSubmission    = native.SendSubmission
	RequestOp         = native.RequestOp
	RequestSubmitOp   = native.RequestSubmitOp
	RequestSubmission = native.RequestSubmission
	ReplyOp           = native.ReplyOp
	ReplySubmitOp     = native.ReplySubmitOp
	PublishOp         = native.PublishOp
	PublishSubmitOp   = native.PublishSubmitOp

	MonitorEventMask     = contracts.MonitorEventMask
	MonitorSourceKind    = contracts.MonitorSourceKind
	MonitorEventType     = contracts.MonitorEventType
	MonitorEventFlag     = contracts.MonitorEventFlag
	MonitorTransportLane = contracts.MonitorTransportLane
	MonitorOpenOption    = contracts.MonitorOpenOption
	MonitorEvent         = contracts.MonitorEvent
	MonitorStatus        = contracts.MonitorStatus
	SocketMonitor        = native.SocketMonitor

	RoutingID         = contracts.RoutingID
	CompletionKind    = contracts.CompletionKind
	Message           = native.Message
	ReplyToken        = native.ReplyToken
	Received          = native.Received
	TopicMessage      = native.TopicMessage
	SubscriptionEvent = native.SubscriptionEvent
	StreamPacket      = native.StreamPacket

	Version               = contracts.Version
	Context               = native.Context
	ContextOptions        = native.ContextOptions
	CoreHwmBudgetSnapshot = contracts.CoreHwmBudgetSnapshot
	AutoHwmProfile        = contracts.AutoHwmProfile
	AutoHwmRecalcReason   = contracts.AutoHwmRecalcReason
	SocketTarget          = native.SocketTarget
	Stopwatch             = native.Stopwatch
	AtomicCounter         = native.AtomicCounter
	Thread                = native.Thread

	SocketType             = contracts.SocketType
	RidDuplicatePolicy     = contracts.RidDuplicatePolicy
	SubmitRetryMode        = contracts.SubmitRetryMode
	ReceiveFlowState       = contracts.ReceiveFlowState
	CommonSocketOptions    = native.CommonSocketOptions
	PubSocketOptions       = native.PubSocketOptions
	PairSocket             = native.PairSocket
	PubSocket              = native.PubSocket
	SubSocket              = native.SubSocket
	DealerSocket           = native.DealerSocket
	RouterSocket           = native.RouterSocket
	RouterRoute            = contracts.RouterRoute
	ConnectionAttempt      = contracts.ConnectionAttempt
	ConnectionFailure      = contracts.ConnectionFailure
	ConnectionAttemptState = contracts.ConnectionAttemptState
	ConnectionFailureKind  = contracts.ConnectionFailureKind
	ConnectionFailureStage = contracts.ConnectionFailureStage
	XPubSocket             = native.XPubSocket
	XSubSocket             = native.XSubSocket
	StreamSocket           = native.StreamSocket
	StreamReceiveMode      = contracts.StreamReceiveMode
	SendFlags              = contracts.SendFlags
	RecvFlags              = contracts.RecvFlags

	PollEventFlag  = contracts.PollEventFlag
	PollSourceKind = contracts.PollSourceKind
	PollItem       = native.PollItem
	PollEvent      = native.PollEvent
	Poller         = native.Poller
	Timer          = native.Timer

	ZlinkError    = contracts.ZlinkError
	SubmitError   = contracts.SubmitError
	RequestError  = contracts.RequestError
	RecvError     = contracts.RecvError
	HandlerError  = contracts.HandlerError
	CloseError    = contracts.CloseError
	BindError     = contracts.BindError
	ConnectError  = contracts.ConnectError
	ConfigError   = contracts.ConfigError
	SubmitResult  = contracts.SubmitResult
	RequestResult = contracts.RequestResult
	RecvResult    = contracts.RecvResult
	HandlerResult = contracts.HandlerResult
	CloseResult   = contracts.CloseResult
	BindResult    = contracts.BindResult
	ConnectResult = contracts.ConnectResult
	ConfigResult  = contracts.ConfigResult
)

const (
	SocketTypeAny    = contracts.SocketTypeAny
	SocketTypePair   = contracts.SocketTypePair
	SocketTypePub    = contracts.SocketTypePub
	SocketTypeSub    = contracts.SocketTypeSub
	SocketTypeDealer = contracts.SocketTypeDealer
	SocketTypeRouter = contracts.SocketTypeRouter
	SocketTypeXPub   = contracts.SocketTypeXPub
	SocketTypeXSub   = contracts.SocketTypeXSub
	SocketTypeStream = contracts.SocketTypeStream

	MonitorEventConnected               = contracts.MonitorEventConnected
	MonitorEventConnectDelayed          = contracts.MonitorEventConnectDelayed
	MonitorEventConnectRetried          = contracts.MonitorEventConnectRetried
	MonitorEventListening               = contracts.MonitorEventListening
	MonitorEventBindFailed              = contracts.MonitorEventBindFailed
	MonitorEventAccepted                = contracts.MonitorEventAccepted
	MonitorEventAcceptFailed            = contracts.MonitorEventAcceptFailed
	MonitorEventClosed                  = contracts.MonitorEventClosed
	MonitorEventCloseFailed             = contracts.MonitorEventCloseFailed
	MonitorEventDisconnected            = contracts.MonitorEventDisconnected
	MonitorEventMonitorStopped          = contracts.MonitorEventMonitorStopped
	MonitorEventHandshakeFailedNoDetail = contracts.MonitorEventHandshakeFailedNoDetail
	MonitorEventConnectionReady         = contracts.MonitorEventConnectionReady
	MonitorEventHandshakeFailedProtocol = contracts.MonitorEventHandshakeFailedProtocol
	MonitorEventHandshakeFailedAuth     = contracts.MonitorEventHandshakeFailedAuth
	MonitorEventPeerWeightChanged       = contracts.MonitorEventPeerWeightChanged
	MonitorEventSendFlowPaused          = contracts.MonitorEventSendFlowPaused
	MonitorEventSendFlowResumed         = contracts.MonitorEventSendFlowResumed
	MonitorEventFlowStateStale          = contracts.MonitorEventFlowStateStale
	MonitorEventAll                     = contracts.MonitorEventAll

	MonitorEventTypeConnected               = contracts.MonitorEventTypeConnected
	MonitorEventTypeConnectDelayed          = contracts.MonitorEventTypeConnectDelayed
	MonitorEventTypeConnectRetried          = contracts.MonitorEventTypeConnectRetried
	MonitorEventTypeListening               = contracts.MonitorEventTypeListening
	MonitorEventTypeBindFailed              = contracts.MonitorEventTypeBindFailed
	MonitorEventTypeAccepted                = contracts.MonitorEventTypeAccepted
	MonitorEventTypeAcceptFailed            = contracts.MonitorEventTypeAcceptFailed
	MonitorEventTypeClosed                  = contracts.MonitorEventTypeClosed
	MonitorEventTypeCloseFailed             = contracts.MonitorEventTypeCloseFailed
	MonitorEventTypeDisconnected            = contracts.MonitorEventTypeDisconnected
	MonitorEventTypeMonitorStopped          = contracts.MonitorEventTypeMonitorStopped
	MonitorEventTypeHandshakeFailedNoDetail = contracts.MonitorEventTypeHandshakeFailedNoDetail
	MonitorEventTypeConnectionReady         = contracts.MonitorEventTypeConnectionReady
	MonitorEventTypeHandshakeFailedProtocol = contracts.MonitorEventTypeHandshakeFailedProtocol
	MonitorEventTypeHandshakeFailedAuth     = contracts.MonitorEventTypeHandshakeFailedAuth
	MonitorEventTypePeerWeightChanged       = contracts.MonitorEventTypePeerWeightChanged
	MonitorEventTypeSendFlowPaused          = contracts.MonitorEventTypeSendFlowPaused
	MonitorEventTypeSendFlowResumed         = contracts.MonitorEventTypeSendFlowResumed
	MonitorEventTypeFlowStateStale          = contracts.MonitorEventTypeFlowStateStale
	MonitorEventTypeAll                     = contracts.MonitorEventTypeAll
	MonitorSourceSocket                     = contracts.MonitorSourceSocket

	MonitorEventFlagConnectionReadyEdge = contracts.MonitorEventFlagConnectionReadyEdge
	MonitorEventFlagSendFlowWritable    = contracts.MonitorEventFlagSendFlowWritable
	MonitorEventFlagFlowStateStaleEpoch = contracts.MonitorEventFlagFlowStateStaleEpoch
	MonitorTransportLaneApplication     = contracts.MonitorTransportLaneApplication
	MonitorTransportLaneCompletion      = contracts.MonitorTransportLaneCompletion

	AutoHwmProfileCompact             = contracts.AutoHwmProfileCompact
	AutoHwmProfileLowLatency          = contracts.AutoHwmProfileLowLatency
	AutoHwmProfileBalanced            = contracts.AutoHwmProfileBalanced
	AutoHwmProfileThroughput          = contracts.AutoHwmProfileThroughput
	AutoHwmRecalcReasonNone           = contracts.AutoHwmRecalcReasonNone
	AutoHwmRecalcReasonInitial        = contracts.AutoHwmRecalcReasonInitial
	AutoHwmRecalcReasonRoleChange     = contracts.AutoHwmRecalcReasonRoleChange
	AutoHwmRecalcReasonPolicyToggle   = contracts.AutoHwmRecalcReasonPolicyToggle
	AutoHwmRecalcReasonRefresh        = contracts.AutoHwmRecalcReasonRefresh
	AutoHwmRecalcReasonDeferredShrink = contracts.AutoHwmRecalcReasonDeferredShrink

	RidDuplicateReject       = contracts.RidDuplicateReject
	RidDuplicateHandover     = contracts.RidDuplicateHandover
	SubmitRetryOff           = contracts.SubmitRetryOff
	SubmitRetryLocalFailure  = contracts.SubmitRetryLocalFailure
	ReceiveFlowRunning       = contracts.ReceiveFlowRunning
	ReceiveFlowPaused        = contracts.ReceiveFlowPaused
	SendFlagsNone            = contracts.SendFlagsNone
	SendFlagsDontWait        = contracts.SendFlagsDontWait
	RecvFlagsNone            = contracts.RecvFlagsNone
	RecvFlagsDontWait        = contracts.RecvFlagsDontWait
	StreamReceiveUnspecified = contracts.StreamReceiveUnspecified
	StreamReceiveRaw         = contracts.StreamReceiveRaw
	StreamReceivePacket      = contracts.StreamReceivePacket
	PollIn                   = contracts.PollIn
	PollOut                  = contracts.PollOut
	PollErr                  = contracts.PollErr
	PollPri                  = contracts.PollPri
	PollCompletion           = contracts.PollCompletion
	PollRoute                = contracts.PollRoute
	PollConnect              = contracts.PollConnect
	PollSourceSocket         = contracts.PollSourceSocket
	PollSourceFD             = contracts.PollSourceFD
	PollSourceTimer          = contracts.PollSourceTimer
	CompletionSend           = contracts.CompletionSend
	CompletionRequest        = contracts.CompletionRequest
	CompletionWritable       = contracts.CompletionWritable

	ConnectionConnecting             = contracts.ConnectionConnecting
	ConnectionConnected              = contracts.ConnectionConnected
	ConnectionAttemptFailed          = contracts.ConnectionAttemptFailed
	ConnectionFailureNone            = contracts.ConnectionFailureNone
	ConnectionFailureRefused         = contracts.ConnectionFailureRefused
	ConnectionFailureEndpointMissing = contracts.ConnectionFailureEndpointMissing
	ConnectionFailureTimedOut        = contracts.ConnectionFailureTimedOut
	ConnectionFailureUnreachable     = contracts.ConnectionFailureUnreachable
	ConnectionFailureNameResolution  = contracts.ConnectionFailureNameResolution
	ConnectionFailureAuthFailed      = contracts.ConnectionFailureAuthFailed
	ConnectionFailureProtocolFailed  = contracts.ConnectionFailureProtocolFailed
	ConnectionFailureConnectionLost  = contracts.ConnectionFailureConnectionLost
	ConnectionFailureOther           = contracts.ConnectionFailureOther
	ConnectionStageNone              = contracts.ConnectionStageNone
	ConnectionStageResolve           = contracts.ConnectionStageResolve
	ConnectionStageTransport         = contracts.ConnectionStageTransport
	ConnectionStageTLS               = contracts.ConnectionStageTLS
	ConnectionStageWebSocket         = contracts.ConnectionStageWebSocket
	ConnectionStageCoreHandshake     = contracts.ConnectionStageCoreHandshake
	ConnectionStageEstablished       = contracts.ConnectionStageEstablished

	SubmitOK              = contracts.SubmitOK
	SubmitBackpressured   = contracts.SubmitBackpressured
	SubmitNotConnected    = contracts.SubmitNotConnected
	SubmitNotFound        = contracts.SubmitNotFound
	SubmitTerminated      = contracts.SubmitTerminated
	SubmitInvalidHandle   = contracts.SubmitInvalidHandle
	SubmitInvalidArgument = contracts.SubmitInvalidArgument
	SubmitNotSupported    = contracts.SubmitNotSupported
	SubmitInvalidState    = contracts.SubmitInvalidState
	SubmitThreadViolation = contracts.SubmitThreadViolation
	SubmitOutOfMemory     = contracts.SubmitOutOfMemory
	SubmitSeqExhausted    = contracts.SubmitSeqExhausted
	SubmitInternalError   = contracts.SubmitInternalError
	SubmitNotAdmitted     = contracts.SubmitNotAdmitted

	RequestOK              = contracts.RequestOK
	RequestTimedOut        = contracts.RequestTimedOut
	RequestNotFound        = contracts.RequestNotFound
	RequestTerminated      = contracts.RequestTerminated
	RequestProtocolError   = contracts.RequestProtocolError
	RequestInternalError   = contracts.RequestInternalError
	RequestRejected        = contracts.RequestRejected
	RequestConflict        = contracts.RequestConflict
	RequestBusy            = contracts.RequestBusy
	RequestNotConnected    = contracts.RequestNotConnected
	RequestInvalidArgument = contracts.RequestInvalidArgument
	RequestInvalidState    = contracts.RequestInvalidState
	RequestNotSupported    = contracts.RequestNotSupported
	RequestBackpressured   = contracts.RequestBackpressured

	RecvOK             = contracts.RecvOK
	RecvNoData         = contracts.RecvNoData
	RecvBusy           = contracts.RecvBusy
	RecvTerminated     = contracts.RecvTerminated
	RecvInvalidHandle  = contracts.RecvInvalidHandle
	RecvNotSupported   = contracts.RecvNotSupported
	RecvInternalError  = contracts.RecvInternalError
	RecvBufferTooSmall = contracts.RecvBufferTooSmall
	RecvInvalidState   = contracts.RecvInvalidState

	HandlerOK              = contracts.HandlerOK
	HandlerInvalidArgument = contracts.HandlerInvalidArgument
	HandlerBusy            = contracts.HandlerBusy
	HandlerNotSupported    = contracts.HandlerNotSupported
	HandlerDeadlock        = contracts.HandlerDeadlock
	HandlerInvalidHandle   = contracts.HandlerInvalidHandle
	HandlerInternalError   = contracts.HandlerInternalError

	CloseOK            = contracts.CloseOK
	CloseBusy          = contracts.CloseBusy
	CloseShutdown      = contracts.CloseShutdown
	CloseInvalidHandle = contracts.CloseInvalidHandle
	CloseInternalError = contracts.CloseInternalError

	BindOK              = contracts.BindOK
	BindInvalidArgument = contracts.BindInvalidArgument
	BindAddrInUse       = contracts.BindAddrInUse
	BindNotSupported    = contracts.BindNotSupported
	BindInvalidHandle   = contracts.BindInvalidHandle
	BindInternalError   = contracts.BindInternalError

	ConnectOK              = contracts.ConnectOK
	ConnectInvalidArgument = contracts.ConnectInvalidArgument
	ConnectNotSupported    = contracts.ConnectNotSupported
	ConnectInvalidHandle   = contracts.ConnectInvalidHandle
	ConnectInternalError   = contracts.ConnectInternalError
	ConnectNotFound        = contracts.ConnectNotFound
	ConnectConflict        = contracts.ConnectConflict
	ConnectBusy            = contracts.ConnectBusy
	ConnectAuthFailed      = contracts.ConnectAuthFailed

	ConfigOK              = contracts.ConfigOK
	ConfigInvalidHandle   = contracts.ConfigInvalidHandle
	ConfigInvalidArgument = contracts.ConfigInvalidArgument
	ConfigNotSupported    = contracts.ConfigNotSupported
	ConfigInternalError   = contracts.ConfigInternalError
	ConfigInvalidState    = contracts.ConfigInvalidState
	ConfigNotFound        = contracts.ConfigNotFound
	ConfigConflict        = contracts.ConfigConflict
	ConfigBufferTooSmall  = contracts.ConfigBufferTooSmall
	ConfigBusy            = contracts.ConfigBusy
)

var (
	MonitorHwmBytes        = contracts.MonitorHwmBytes
	OpenSocketMonitor      = native.OpenSocketMonitor
	NewRoutingID           = contracts.NewRoutingID
	NewRoutingIDString     = contracts.NewRoutingIDString
	NewRoutingIDUint32     = contracts.NewRoutingIDUint32
	NewRoutingIDUUIDBytes  = contracts.NewRoutingIDUUIDBytes
	NewRoutingIDFromHex    = contracts.NewRoutingIDFromHex
	TryNewRoutingID        = contracts.TryNewRoutingID
	TryNewRoutingIDFromHex = contracts.TryNewRoutingIDFromHex
	NewMessage             = native.NewMessage
	NewMessageString       = native.NewMessageString
	NewMessageWithSize     = native.NewMessageWithSize
	RuntimeVersion         = native.RuntimeVersion
	NewContext             = native.NewContext
	NewTimer               = native.NewTimer
	NewPoller              = native.NewPoller
	Poll                   = native.Poll
	Has                    = native.Has
	Proxy                  = native.Proxy
	Sleep                  = native.Sleep
	MultipartClose         = native.MultipartClose
	NewStopwatch           = native.NewStopwatch
	NewAtomicCounter       = native.NewAtomicCounter
	NewThread              = native.NewThread
)
