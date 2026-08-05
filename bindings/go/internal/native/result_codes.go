// SPDX-License-Identifier: MPL-2.0

package native

type SendFlags int

const (
	SendFlagsNone     SendFlags = 0
	SendFlagsDontWait SendFlags = 1
)

type RecvFlags int

const (
	RecvFlagsNone     RecvFlags = 0
	RecvFlagsDontWait RecvFlags = 1
)

type SubmitResult int

const (
	SubmitOK              SubmitResult = 0
	SubmitBackpressured   SubmitResult = 1
	SubmitNotConnected    SubmitResult = 2
	SubmitNotFound        SubmitResult = 3
	SubmitTerminated      SubmitResult = 4
	SubmitInvalidHandle   SubmitResult = 5
	SubmitInvalidArgument SubmitResult = 6
	SubmitNotSupported    SubmitResult = 7
	SubmitInvalidState    SubmitResult = 8
	SubmitThreadViolation SubmitResult = 9
	SubmitOutOfMemory     SubmitResult = 10
	SubmitSeqExhausted    SubmitResult = 11
	SubmitInternalError   SubmitResult = 12
	SubmitNotAdmitted     SubmitResult = 13
)

type RequestResult int

const (
	RequestOK              RequestResult = 0
	RequestTimedOut        RequestResult = 101
	RequestNotFound        RequestResult = 102
	RequestTerminated      RequestResult = 103
	RequestProtocolError   RequestResult = 104
	RequestInternalError   RequestResult = 105
	RequestRejected        RequestResult = 106
	RequestConflict        RequestResult = 107
	RequestBusy            RequestResult = 108
	RequestNotConnected    RequestResult = 109
	RequestInvalidArgument RequestResult = 110
	RequestInvalidState    RequestResult = 111
	RequestNotSupported    RequestResult = 112
	RequestBackpressured   RequestResult = 113
)

type RecvResult int

const (
	RecvOK             RecvResult = 0
	RecvNoData         RecvResult = 201
	RecvBusy           RecvResult = 202
	RecvTerminated     RecvResult = 203
	RecvInvalidHandle  RecvResult = 204
	RecvNotSupported   RecvResult = 205
	RecvInternalError  RecvResult = 206
	RecvBufferTooSmall RecvResult = 207
	RecvInvalidState   RecvResult = 208
)

type HandlerResult int

const (
	HandlerOK              HandlerResult = 0
	HandlerInvalidArgument HandlerResult = 301
	HandlerBusy            HandlerResult = 302
	HandlerNotSupported    HandlerResult = 303
	HandlerDeadlock        HandlerResult = 304
	HandlerInvalidHandle   HandlerResult = 305
	HandlerInternalError   HandlerResult = 306
)

type CloseResult int

const (
	CloseOK            CloseResult = 0
	CloseBusy          CloseResult = 401
	CloseShutdown      CloseResult = 402
	CloseInvalidHandle CloseResult = 403
	CloseInternalError CloseResult = 404
)

type BindResult int

const (
	BindOK              BindResult = 0
	BindInvalidArgument BindResult = 501
	BindAddrInUse       BindResult = 502
	BindNotSupported    BindResult = 503
	BindInvalidHandle   BindResult = 504
	BindInternalError   BindResult = 505
)

type ConnectResult int

const (
	ConnectOK              ConnectResult = 0
	ConnectInvalidArgument ConnectResult = 601
	ConnectNotSupported    ConnectResult = 602
	ConnectInvalidHandle   ConnectResult = 603
	ConnectInternalError   ConnectResult = 604
	ConnectNotFound        ConnectResult = 605
	ConnectConflict        ConnectResult = 606
	ConnectBusy            ConnectResult = 607
	ConnectAuthFailed      ConnectResult = 608
)

type ConfigResult int

const (
	ConfigOK              ConfigResult = 0
	ConfigInvalidHandle   ConfigResult = 701
	ConfigInvalidArgument ConfigResult = 702
	ConfigNotSupported    ConfigResult = 703
	ConfigInternalError   ConfigResult = 704
	ConfigInvalidState    ConfigResult = 705
	ConfigNotFound        ConfigResult = 706
	ConfigConflict        ConfigResult = 707
	ConfigBufferTooSmall  ConfigResult = 708
	ConfigBusy            ConfigResult = 709
)
