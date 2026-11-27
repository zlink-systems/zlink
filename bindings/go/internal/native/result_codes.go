// SPDX-License-Identifier: MPL-2.0

package native

import contracts "zlink.systems/zlink/contracts"

type (
	SendFlags = contracts.SendFlags
	RecvFlags = contracts.RecvFlags
)

type CompletionKind = contracts.CompletionKind

const (
	CompletionSend     = contracts.CompletionSend
	CompletionRequest  = contracts.CompletionRequest
	CompletionWritable = contracts.CompletionWritable
)

type SendCompleteResult int

const (
	SendAdmitted     SendCompleteResult = 0
	SendNotFound     SendCompleteResult = 801
	SendNotConnected SendCompleteResult = 802
	SendTimedOut     SendCompleteResult = 803
)

// Result codes are owned by contracts and shared with the native binding.
type (
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
	SubmitOK               = contracts.SubmitOK
	SubmitBackpressured    = contracts.SubmitBackpressured
	SubmitNotConnected     = contracts.SubmitNotConnected
	SubmitNotFound         = contracts.SubmitNotFound
	SubmitTerminated       = contracts.SubmitTerminated
	SubmitInvalidHandle    = contracts.SubmitInvalidHandle
	SubmitInvalidArgument  = contracts.SubmitInvalidArgument
	SubmitNotSupported     = contracts.SubmitNotSupported
	SubmitInvalidState     = contracts.SubmitInvalidState
	SubmitThreadViolation  = contracts.SubmitThreadViolation
	SubmitOutOfMemory      = contracts.SubmitOutOfMemory
	SubmitSeqExhausted     = contracts.SubmitSeqExhausted
	SubmitInternalError    = contracts.SubmitInternalError
	SubmitNotAdmitted      = contracts.SubmitNotAdmitted
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
	RecvOK                 = contracts.RecvOK
	RecvNoData             = contracts.RecvNoData
	RecvBusy               = contracts.RecvBusy
	RecvTerminated         = contracts.RecvTerminated
	RecvInvalidHandle      = contracts.RecvInvalidHandle
	RecvNotSupported       = contracts.RecvNotSupported
	RecvInternalError      = contracts.RecvInternalError
	RecvBufferTooSmall     = contracts.RecvBufferTooSmall
	RecvInvalidState       = contracts.RecvInvalidState
	HandlerOK              = contracts.HandlerOK
	HandlerInvalidArgument = contracts.HandlerInvalidArgument
	HandlerBusy            = contracts.HandlerBusy
	HandlerNotSupported    = contracts.HandlerNotSupported
	HandlerDeadlock        = contracts.HandlerDeadlock
	HandlerInvalidHandle   = contracts.HandlerInvalidHandle
	HandlerInternalError   = contracts.HandlerInternalError
	CloseOK                = contracts.CloseOK
	CloseBusy              = contracts.CloseBusy
	CloseShutdown          = contracts.CloseShutdown
	CloseInvalidHandle     = contracts.CloseInvalidHandle
	CloseInternalError     = contracts.CloseInternalError
	BindOK                 = contracts.BindOK
	BindInvalidArgument    = contracts.BindInvalidArgument
	BindAddrInUse          = contracts.BindAddrInUse
	BindNotSupported       = contracts.BindNotSupported
	BindInvalidHandle      = contracts.BindInvalidHandle
	BindInternalError      = contracts.BindInternalError
	ConnectOK              = contracts.ConnectOK
	ConnectInvalidArgument = contracts.ConnectInvalidArgument
	ConnectNotSupported    = contracts.ConnectNotSupported
	ConnectInvalidHandle   = contracts.ConnectInvalidHandle
	ConnectInternalError   = contracts.ConnectInternalError
	ConnectNotFound        = contracts.ConnectNotFound
	ConnectConflict        = contracts.ConnectConflict
	ConnectBusy            = contracts.ConnectBusy
	ConnectAuthFailed      = contracts.ConnectAuthFailed
	ConfigOK               = contracts.ConfigOK
	ConfigInvalidHandle    = contracts.ConfigInvalidHandle
	ConfigInvalidArgument  = contracts.ConfigInvalidArgument
	ConfigNotSupported     = contracts.ConfigNotSupported
	ConfigInternalError    = contracts.ConfigInternalError
	ConfigInvalidState     = contracts.ConfigInvalidState
	ConfigNotFound         = contracts.ConfigNotFound
	ConfigConflict         = contracts.ConfigConflict
	ConfigBufferTooSmall   = contracts.ConfigBufferTooSmall
	ConfigBusy             = contracts.ConfigBusy
)
