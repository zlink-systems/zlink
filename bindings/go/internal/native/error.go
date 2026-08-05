// SPDX-License-Identifier: MPL-2.0

package native

/*
#include "zlink.h"
*/
import "C"

import (
	"errors"
	"fmt"
	"syscall"
)

type ZlinkError interface {
	error
	Code() int
	InternalErrno() int
}

type SubmitError struct {
	Result      SubmitResult
	nativeErrno int
}

type RequestError struct {
	Result      RequestResult
	nativeErrno int
}

type RecvError struct {
	Result      RecvResult
	nativeErrno int
}

// isNoData reports whether err is a RecvError carrying the no-data result —
// the signal a non-blocking receive raises when nothing was available.
func isNoData(err error) bool {
	var recvErr *RecvError
	return errors.As(err, &recvErr) && recvErr.Result == RecvNoData
}

type HandlerError struct {
	Result      HandlerResult
	nativeErrno int
}

type CloseError struct {
	Result      CloseResult
	nativeErrno int
}

type BindError struct {
	Result      BindResult
	nativeErrno int
}

type ConnectError struct {
	Result      ConnectResult
	nativeErrno int
}

type ConfigError struct {
	Result      ConfigResult
	nativeErrno int
}

func (e *SubmitError) Error() string {
	return formatError("submit", int(e.Result), e.internalErrno())
}

func (e *SubmitError) Code() int { return int(e.Result) }

func (e *SubmitError) InternalErrno() int { return e.internalErrno() }

func (e *SubmitError) Unwrap() error { return errnoToError(e.internalErrno()) }

func (e *RequestError) Error() string {
	return formatError("request", int(e.Result), e.internalErrno())
}

func (e *RequestError) Code() int { return int(e.Result) }

func (e *RequestError) InternalErrno() int { return e.internalErrno() }

func (e *RequestError) Unwrap() error { return errnoToError(e.internalErrno()) }

func (e *RecvError) Error() string {
	return formatError("recv", int(e.Result), e.internalErrno())
}

func (e *RecvError) Code() int { return int(e.Result) }

func (e *RecvError) InternalErrno() int { return e.internalErrno() }

func (e *RecvError) Unwrap() error { return errnoToError(e.internalErrno()) }

func (e *HandlerError) Error() string {
	return formatError("handler", int(e.Result), e.internalErrno())
}

func (e *HandlerError) Code() int { return int(e.Result) }

func (e *HandlerError) InternalErrno() int { return e.internalErrno() }

func (e *HandlerError) Unwrap() error { return errnoToError(e.internalErrno()) }

func (e *CloseError) Error() string {
	return formatError("close", int(e.Result), e.internalErrno())
}

func (e *CloseError) Code() int { return int(e.Result) }

func (e *CloseError) InternalErrno() int { return e.internalErrno() }

func (e *CloseError) Unwrap() error { return errnoToError(e.internalErrno()) }

func (e *BindError) Error() string {
	return formatError("bind", int(e.Result), e.internalErrno())
}

func (e *BindError) Code() int { return int(e.Result) }

func (e *BindError) InternalErrno() int { return e.internalErrno() }

func (e *BindError) Unwrap() error { return errnoToError(e.internalErrno()) }

func (e *ConnectError) Error() string {
	return formatError("connect", int(e.Result), e.internalErrno())
}

func (e *ConnectError) Code() int { return int(e.Result) }

func (e *ConnectError) InternalErrno() int { return e.internalErrno() }

func (e *ConnectError) Unwrap() error { return errnoToError(e.internalErrno()) }

func (e *ConfigError) Error() string {
	return formatError("config", int(e.Result), e.internalErrno())
}

func (e *ConfigError) Code() int { return int(e.Result) }

func (e *ConfigError) InternalErrno() int { return e.internalErrno() }

func (e *ConfigError) Unwrap() error { return errnoToError(e.internalErrno()) }

func (e *SubmitError) internalErrno() int {
	if e == nil {
		return 0
	}
	return e.nativeErrno
}

func (e *RequestError) internalErrno() int {
	if e == nil {
		return 0
	}
	return e.nativeErrno
}

func (e *RecvError) internalErrno() int {
	if e == nil {
		return 0
	}
	return e.nativeErrno
}

func (e *HandlerError) internalErrno() int {
	if e == nil {
		return 0
	}
	return e.nativeErrno
}

func (e *CloseError) internalErrno() int {
	if e == nil {
		return 0
	}
	return e.nativeErrno
}

func (e *BindError) internalErrno() int {
	if e == nil {
		return 0
	}
	return e.nativeErrno
}

func (e *ConnectError) internalErrno() int {
	if e == nil {
		return 0
	}
	return e.nativeErrno
}

func (e *ConfigError) internalErrno() int {
	if e == nil {
		return 0
	}
	return e.nativeErrno
}

func formatError(kind string, code int, nativeErrno int) string {
	if nativeErrno != 0 {
		return fmt.Sprintf("%s error (%d): %s", kind, code, C.GoString(C.zlink_strerror(C.int(nativeErrno))))
	}
	return fmt.Sprintf("%s error (%d)", kind, code)
}

func errnoToError(errno int) error {
	if errno == 0 {
		return nil
	}
	return syscall.Errno(errno)
}

func currentErrno() int {
	return int(C.zlink_errno())
}

func errnoOrIO() int {
	if errno := currentErrno(); errno != 0 {
		return errno
	}
	return int(C.EIO)
}

func fallbackSubmitErrno(result SubmitResult) int {
	switch result {
	case SubmitBackpressured, SubmitNotAdmitted:
		return int(C.EAGAIN)
	case SubmitNotConnected, SubmitNotFound:
		return int(C.ENOTCONN)
	case SubmitTerminated:
		return int(C.ETERM)
	case SubmitInvalidHandle:
		return int(C.EFAULT)
	case SubmitInvalidArgument:
		return int(C.EINVAL)
	case SubmitNotSupported:
		return int(C.ENOTSUP)
	case SubmitInvalidState, SubmitThreadViolation:
		return int(C.EBUSY)
	case SubmitOutOfMemory:
		return int(C.ENOMEM)
	default:
		return int(C.EIO)
	}
}

func fallbackRequestErrno(result RequestResult) int {
	switch result {
	case RequestBackpressured:
		return int(C.EAGAIN)
	case RequestTimedOut:
		return int(C.ETIMEDOUT)
	case RequestNotFound:
		return int(C.ENOENT)
	case RequestTerminated:
		return int(C.ETERM)
	case RequestInternalError:
		return int(C.EIO)
	case RequestRejected:
		return int(C.ECONNREFUSED)
	case RequestConflict, RequestInvalidArgument, RequestInvalidState:
		return int(C.EINVAL)
	case RequestBusy:
		return int(C.EBUSY)
	case RequestNotConnected:
		return int(C.ENOTCONN)
	case RequestNotSupported:
		return int(C.ENOTSUP)
	default:
		return int(C.EPROTO)
	}
}

func fallbackRecvErrno(result RecvResult) int {
	switch result {
	case RecvNoData:
		return int(C.EAGAIN)
	case RecvBusy:
		return int(C.EBUSY)
	case RecvTerminated:
		return int(C.ETERM)
	case RecvInvalidHandle:
		return int(C.EFAULT)
	case RecvNotSupported:
		return int(C.ENOTSUP)
	case RecvInternalError:
		return int(C.EINTR)
	case RecvBufferTooSmall:
		return int(C.ENOBUFS)
	case RecvInvalidState:
		return int(C.EINVAL)
	default:
		return int(C.EIO)
	}
}

type resultCodeValue interface {
	~int | ~int8 | ~int16 | ~int32 | ~int64 |
		~uint | ~uint8 | ~uint16 | ~uint32 | ~uint64 | ~uintptr
}

func submitErrorFromResult[T resultCodeValue](result T) error {
	resultCode := SubmitResult(resultCodeInt(result))
	if resultCode == SubmitOK {
		return nil
	}
	errno := currentErrno()
	if errno == 0 {
		errno = fallbackSubmitErrno(resultCode)
	}
	return &SubmitError{Result: resultCode, nativeErrno: errno}
}

// requestCompletionError maps a terminal callback result without consulting
// thread-local errno. A callback carries a result code, not the errno from the
// native thread that produced it, so completion errors must use the stable
// result-to-errno policy.
func requestCompletionError(result RequestResult) error {
	if result == RequestOK {
		return nil
	}
	return &RequestError{Result: result, nativeErrno: fallbackRequestErrno(result)}
}

func recvErrorFromResult[T resultCodeValue](result T) error {
	resultCode := RecvResult(resultCodeInt(result))
	if resultCode == RecvOK {
		return nil
	}
	errno := currentErrno()
	if errno == 0 {
		errno = fallbackRecvErrno(resultCode)
	}
	return &RecvError{Result: resultCode, nativeErrno: errno}
}

func handlerErrorFromResult[T resultCodeValue](result T) error {
	if resultCodeInt(result) == int(HandlerOK) {
		return nil
	}
	errno := errnoOrIO()
	return &HandlerError{Result: HandlerResult(resultCodeInt(result)), nativeErrno: errno}
}

func closeErrorFromResult[T resultCodeValue](result T) error {
	if resultCodeInt(result) == int(CloseOK) {
		return nil
	}
	errno := errnoOrIO()
	return &CloseError{Result: CloseResult(resultCodeInt(result)), nativeErrno: errno}
}

func bindErrorFromResult[T resultCodeValue](result T) error {
	if resultCodeInt(result) == int(BindOK) {
		return nil
	}
	errno := errnoOrIO()
	return &BindError{Result: BindResult(resultCodeInt(result)), nativeErrno: errno}
}

func connectErrorFromResult[T resultCodeValue](result T) error {
	if resultCodeInt(result) == int(ConnectOK) {
		return nil
	}
	errno := errnoOrIO()
	return &ConnectError{Result: ConnectResult(resultCodeInt(result)), nativeErrno: errno}
}

func configErrorFromResult[T resultCodeValue](result T) error {
	if resultCodeInt(result) == int(ConfigOK) {
		return nil
	}
	errno := errnoOrIO()
	return &ConfigError{Result: ConfigResult(resultCodeInt(result)), nativeErrno: errno}
}

func validationError(format string, args ...any) error {
	return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
}

func stateError(format string, args ...any) error {
	return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
}

func configErrorFromErrno(errno int) error {
	switch errno {
	case 0:
		return nil
	case int(C.EFAULT), int(C.ENOTSOCK), int(C.EDESTADDRREQ):
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: errno}
	case int(C.EINVAL), int(C.EMSGSIZE), int(C.EAFNOSUPPORT), int(C.ENAMETOOLONG):
		return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: errno}
	case int(C.ENOTSUP):
		return &ConfigError{Result: ConfigNotSupported, nativeErrno: errno}
	default:
		return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: errno}
	}
}

func resultCodeInt[T resultCodeValue](value T) int {
	return int(value)
}
