// SPDX-License-Identifier: MPL-2.0

package contracts

import impl "zlink.systems/zlink/v11/internal/native"

type (
	// ZlinkError is the base error type for the zlink bindings. It exposes the
	// binding result code and supports errors.Is through its internal errno.
	ZlinkError = impl.ZlinkError
	// SubmitError is returned when submitting a send or publish fails.
	SubmitError = impl.SubmitError
	// RequestError is returned when a request fails or its reply reports an error.
	RequestError = impl.RequestError
	// RecvError is returned when receiving a message fails.
	RecvError = impl.RecvError
	// HandlerError is returned when registering or running a callback handler fails.
	HandlerError = impl.HandlerError
	// CloseError is returned when closing a socket or resource fails.
	CloseError = impl.CloseError
	// BindError is returned when binding a socket to an endpoint fails.
	BindError = impl.BindError
	// ConnectError is returned when connecting a socket to an endpoint fails.
	ConnectError = impl.ConnectError
	// ConfigError is returned when reading or applying a configuration option fails.
	ConfigError = impl.ConfigError
	// SubmitResult is the outcome of submitting a send or publish.
	SubmitResult = impl.SubmitResult
	// RequestResult is the outcome of a request.
	RequestResult = impl.RequestResult
	// RecvResult is the outcome of a receive.
	RecvResult = impl.RecvResult
	// HandlerResult is the outcome of registering or running a callback handler.
	HandlerResult = impl.HandlerResult
	// CloseResult is the outcome of closing a socket or resource.
	CloseResult = impl.CloseResult
	// BindResult is the outcome of binding to an endpoint.
	BindResult = impl.BindResult
	// ConnectResult is the outcome of connecting to an endpoint.
	ConnectResult = impl.ConnectResult
	// ConfigResult is the outcome of reading or applying a configuration option.
	ConfigResult = impl.ConfigResult
)

const (
	SubmitOK               = impl.SubmitOK
	SubmitBackpressured    = impl.SubmitBackpressured
	SubmitNotConnected     = impl.SubmitNotConnected
	SubmitNotFound         = impl.SubmitNotFound
	SubmitTerminated       = impl.SubmitTerminated
	SubmitInvalidHandle    = impl.SubmitInvalidHandle
	SubmitInvalidArgument  = impl.SubmitInvalidArgument
	SubmitNotSupported     = impl.SubmitNotSupported
	SubmitInvalidState     = impl.SubmitInvalidState
	SubmitThreadViolation  = impl.SubmitThreadViolation
	SubmitOutOfMemory      = impl.SubmitOutOfMemory
	SubmitSeqExhausted     = impl.SubmitSeqExhausted
	SubmitInternalError    = impl.SubmitInternalError
	SubmitNotAdmitted      = impl.SubmitNotAdmitted
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
	RecvOK                 = impl.RecvOK
	RecvNoData             = impl.RecvNoData
	RecvBusy               = impl.RecvBusy
	RecvTerminated         = impl.RecvTerminated
	RecvInvalidHandle      = impl.RecvInvalidHandle
	RecvNotSupported       = impl.RecvNotSupported
	RecvInternalError      = impl.RecvInternalError
	RecvBufferTooSmall     = impl.RecvBufferTooSmall
	RecvInvalidState       = impl.RecvInvalidState
	HandlerOK              = impl.HandlerOK
	HandlerInvalidArgument = impl.HandlerInvalidArgument
	HandlerBusy            = impl.HandlerBusy
	HandlerNotSupported    = impl.HandlerNotSupported
	HandlerDeadlock        = impl.HandlerDeadlock
	HandlerInvalidHandle   = impl.HandlerInvalidHandle
	HandlerInternalError   = impl.HandlerInternalError
	CloseOK                = impl.CloseOK
	CloseBusy              = impl.CloseBusy
	CloseShutdown          = impl.CloseShutdown
	CloseInvalidHandle     = impl.CloseInvalidHandle
	CloseInternalError     = impl.CloseInternalError
	BindOK                 = impl.BindOK
	BindInvalidArgument    = impl.BindInvalidArgument
	BindAddrInUse          = impl.BindAddrInUse
	BindNotSupported       = impl.BindNotSupported
	BindInvalidHandle      = impl.BindInvalidHandle
	BindInternalError      = impl.BindInternalError
	ConnectOK              = impl.ConnectOK
	ConnectInvalidArgument = impl.ConnectInvalidArgument
	ConnectNotSupported    = impl.ConnectNotSupported
	ConnectInvalidHandle   = impl.ConnectInvalidHandle
	ConnectInternalError   = impl.ConnectInternalError
	ConnectNotFound        = impl.ConnectNotFound
	ConnectConflict        = impl.ConnectConflict
	ConnectBusy            = impl.ConnectBusy
	ConnectAuthFailed      = impl.ConnectAuthFailed
	ConfigOK               = impl.ConfigOK
	ConfigInvalidHandle    = impl.ConfigInvalidHandle
	ConfigInvalidArgument  = impl.ConfigInvalidArgument
	ConfigNotSupported     = impl.ConfigNotSupported
	ConfigInternalError    = impl.ConfigInternalError
	ConfigInvalidState     = impl.ConfigInvalidState
	ConfigNotFound         = impl.ConfigNotFound
	ConfigConflict         = impl.ConfigConflict
	ConfigBufferTooSmall   = impl.ConfigBufferTooSmall
	ConfigBusy             = impl.ConfigBusy
)
