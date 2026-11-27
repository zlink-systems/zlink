// SPDX-License-Identifier: MPL-2.0

package contracts

import (
	"fmt"
	"syscall"
)

// ZlinkError exposes a binding result code and its normalized errno.
type ZlinkError interface {
	error
	Code() int
	InternalErrno() int
}

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

func formatError(kind string, code int, errno int, reason func(int) string) string {
	if errno != 0 {
		var detail string
		if reason != nil {
			detail = reason(errno)
		} else {
			detail = syscall.Errno(errno).Error()
		}
		return fmt.Sprintf("%s error (%d): %s", kind, code, detail)
	}
	return fmt.Sprintf("%s error (%d)", kind, code)
}

func unwrapErrno(errno int) error {
	if errno == 0 {
		return nil
	}
	return syscall.Errno(errno)
}

// SubmitError reports a submit operation failure.
type SubmitError struct {
	Result      SubmitResult
	nativeErrno int
	reason      func(int) string
}

// NewSubmitError retains the native result, normalized errno, and a deferred description.
func NewSubmitError(result SubmitResult, errno int, reason func(int) string) *SubmitError {
	return &SubmitError{Result: result, nativeErrno: errno, reason: reason}
}

func (e *SubmitError) Error() string {
	return formatError("submit", int(e.Result), e.nativeErrno, e.reason)
}

func (e *SubmitError) Code() int { return int(e.Result) }

func (e *SubmitError) InternalErrno() int {
	if e == nil {
		return 0
	}
	return e.nativeErrno
}

func (e *SubmitError) Unwrap() error { return unwrapErrno(e.InternalErrno()) }

// RequestError reports a request operation failure.
type RequestError struct {
	Result      RequestResult
	nativeErrno int
	reason      func(int) string
}

// NewRequestError retains the native result, normalized errno, and a deferred description.
func NewRequestError(result RequestResult, errno int, reason func(int) string) *RequestError {
	return &RequestError{Result: result, nativeErrno: errno, reason: reason}
}

func (e *RequestError) Error() string {
	return formatError("request", int(e.Result), e.nativeErrno, e.reason)
}

func (e *RequestError) Code() int { return int(e.Result) }

func (e *RequestError) InternalErrno() int {
	if e == nil {
		return 0
	}
	return e.nativeErrno
}

func (e *RequestError) Unwrap() error { return unwrapErrno(e.InternalErrno()) }

// RecvError reports a recv operation failure.
type RecvError struct {
	Result      RecvResult
	nativeErrno int
	reason      func(int) string
}

// NewRecvError retains the native result, normalized errno, and a deferred description.
func NewRecvError(result RecvResult, errno int, reason func(int) string) *RecvError {
	return &RecvError{Result: result, nativeErrno: errno, reason: reason}
}

func (e *RecvError) Error() string {
	return formatError("recv", int(e.Result), e.nativeErrno, e.reason)
}

func (e *RecvError) Code() int { return int(e.Result) }

func (e *RecvError) InternalErrno() int {
	if e == nil {
		return 0
	}
	return e.nativeErrno
}

func (e *RecvError) Unwrap() error { return unwrapErrno(e.InternalErrno()) }

// HandlerError reports a handler operation failure.
type HandlerError struct {
	Result      HandlerResult
	nativeErrno int
	reason      func(int) string
}

// NewHandlerError retains the native result, normalized errno, and a deferred description.
func NewHandlerError(result HandlerResult, errno int, reason func(int) string) *HandlerError {
	return &HandlerError{Result: result, nativeErrno: errno, reason: reason}
}

func (e *HandlerError) Error() string {
	return formatError("handler", int(e.Result), e.nativeErrno, e.reason)
}

func (e *HandlerError) Code() int { return int(e.Result) }

func (e *HandlerError) InternalErrno() int {
	if e == nil {
		return 0
	}
	return e.nativeErrno
}

func (e *HandlerError) Unwrap() error { return unwrapErrno(e.InternalErrno()) }

// CloseError reports a close operation failure.
type CloseError struct {
	Result      CloseResult
	nativeErrno int
	reason      func(int) string
}

// NewCloseError retains the native result, normalized errno, and a deferred description.
func NewCloseError(result CloseResult, errno int, reason func(int) string) *CloseError {
	return &CloseError{Result: result, nativeErrno: errno, reason: reason}
}

func (e *CloseError) Error() string {
	return formatError("close", int(e.Result), e.nativeErrno, e.reason)
}

func (e *CloseError) Code() int { return int(e.Result) }

func (e *CloseError) InternalErrno() int {
	if e == nil {
		return 0
	}
	return e.nativeErrno
}

func (e *CloseError) Unwrap() error { return unwrapErrno(e.InternalErrno()) }

// BindError reports a bind operation failure.
type BindError struct {
	Result      BindResult
	nativeErrno int
	reason      func(int) string
}

// NewBindError retains the native result, normalized errno, and a deferred description.
func NewBindError(result BindResult, errno int, reason func(int) string) *BindError {
	return &BindError{Result: result, nativeErrno: errno, reason: reason}
}

func (e *BindError) Error() string {
	return formatError("bind", int(e.Result), e.nativeErrno, e.reason)
}

func (e *BindError) Code() int { return int(e.Result) }

func (e *BindError) InternalErrno() int {
	if e == nil {
		return 0
	}
	return e.nativeErrno
}

func (e *BindError) Unwrap() error { return unwrapErrno(e.InternalErrno()) }

// ConnectError reports a connect operation failure.
type ConnectError struct {
	Result      ConnectResult
	nativeErrno int
	reason      func(int) string
}

// NewConnectError retains the native result, normalized errno, and a deferred description.
func NewConnectError(result ConnectResult, errno int, reason func(int) string) *ConnectError {
	return &ConnectError{Result: result, nativeErrno: errno, reason: reason}
}

func (e *ConnectError) Error() string {
	return formatError("connect", int(e.Result), e.nativeErrno, e.reason)
}

func (e *ConnectError) Code() int { return int(e.Result) }

func (e *ConnectError) InternalErrno() int {
	if e == nil {
		return 0
	}
	return e.nativeErrno
}

func (e *ConnectError) Unwrap() error { return unwrapErrno(e.InternalErrno()) }

// ConfigError reports a config operation failure.
type ConfigError struct {
	Result      ConfigResult
	nativeErrno int
	reason      func(int) string
}

// NewConfigError retains the native result, normalized errno, and a deferred description.
func NewConfigError(result ConfigResult, errno int, reason func(int) string) *ConfigError {
	return &ConfigError{Result: result, nativeErrno: errno, reason: reason}
}

func (e *ConfigError) Error() string {
	return formatError("config", int(e.Result), e.nativeErrno, e.reason)
}

func (e *ConfigError) Code() int { return int(e.Result) }

func (e *ConfigError) InternalErrno() int {
	if e == nil {
		return 0
	}
	return e.nativeErrno
}

func (e *ConfigError) Unwrap() error { return unwrapErrno(e.InternalErrno()) }
