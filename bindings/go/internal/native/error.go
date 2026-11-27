// SPDX-License-Identifier: MPL-2.0

package native

/*
#include "zlink.h"
*/
import "C"

import (
	"errors"
	"syscall"

	contracts "zlink.systems/zlink/contracts"
)

type (
	ZlinkError   = contracts.ZlinkError
	SubmitError  = contracts.SubmitError
	RequestError = contracts.RequestError
	RecvError    = contracts.RecvError
	HandlerError = contracts.HandlerError
	CloseError   = contracts.CloseError
	BindError    = contracts.BindError
	ConnectError = contracts.ConnectError
	ConfigError  = contracts.ConfigError
)

// isNoData reports whether a non-blocking receive found no message.
func isNoData(err error) bool {
	var recvErr *RecvError
	return errors.As(err, &recvErr) && recvErr.Result == RecvNoData
}

// nativeStrerror formats a captured errno only when Error() is called.
func nativeStrerror(errno int) string {
	return C.GoString(C.zlink_strerror(C.int(denormalizeNativeErrno(errno))))
}

func newSubmitError(result SubmitResult, errno int) *SubmitError {
	return contracts.NewSubmitError(result, normalizeNativeErrno(errno), nativeStrerror)
}

func newRequestError(result RequestResult, errno int) *RequestError {
	return contracts.NewRequestError(result, normalizeNativeErrno(errno), nativeStrerror)
}

func newRecvError(result RecvResult, errno int) *RecvError {
	return contracts.NewRecvError(result, normalizeNativeErrno(errno), nativeStrerror)
}

func newHandlerError(result HandlerResult, errno int) *HandlerError {
	return contracts.NewHandlerError(result, normalizeNativeErrno(errno), nativeStrerror)
}

func newCloseError(result CloseResult, errno int) *CloseError {
	return contracts.NewCloseError(result, normalizeNativeErrno(errno), nativeStrerror)
}

func newBindError(result BindResult, errno int) *BindError {
	return contracts.NewBindError(result, normalizeNativeErrno(errno), nativeStrerror)
}

func newConnectError(result ConnectResult, errno int) *ConnectError {
	return contracts.NewConnectError(result, normalizeNativeErrno(errno), nativeStrerror)
}

func newConfigError(result ConfigResult, errno int) *ConfigError {
	return contracts.NewConfigError(result, normalizeNativeErrno(errno), nativeStrerror)
}

// cgoErrno returns the errno that the cgo two-value call form captured on the
// thread that ran the call (rule: never read errno in a later cgo call).
func cgoErrno(cerr error) int {
	if errno, ok := cerr.(syscall.Errno); ok {
		return int(errno)
	}
	return 0
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
		return int(C.EACCES)
	case RequestConflict:
		return int(C.EEXIST)
	case RequestInvalidArgument:
		return int(C.EINVAL)
	case RequestInvalidState:
		return int(C.EFSM)
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

type resultCodeValue interface {
	~int | ~int8 | ~int16 | ~int32 | ~int64 |
		~uint | ~uint8 | ~uint16 | ~uint32 | ~uint64 | ~uintptr
}

func submitErrorFromCall[T resultCodeValue](result T, cerr error) error {
	resultCode := SubmitResult(resultCodeInt(result))
	if resultCode == SubmitOK {
		return nil
	}
	return newSubmitError(resultCode, cgoErrno(cerr))
}

// requestCompletionError maps a terminal completion result without consulting
// thread-local errno. A completion record carries a result code, not the errno
// from the native thread that produced it, so errors must use the stable
// result-to-errno policy.
func requestCompletionError(result RequestResult) error {
	if result == RequestOK {
		return nil
	}
	return newRequestError(result, fallbackRequestErrno(result))
}

func recvErrorFromCall[T resultCodeValue](result T, cerr error) error {
	resultCode := RecvResult(resultCodeInt(result))
	if resultCode == RecvOK {
		return nil
	}
	return newRecvError(resultCode, cgoErrno(cerr))
}

func handlerErrorFromCall[T resultCodeValue](result T, cerr error) error {
	if resultCodeInt(result) == int(HandlerOK) {
		return nil
	}
	errno := cgoErrno(cerr)
	return newHandlerError(HandlerResult(resultCodeInt(result)), errno)
}

func closeErrorFromCall[T resultCodeValue](result T, cerr error) error {
	if resultCodeInt(result) == int(CloseOK) {
		return nil
	}
	errno := cgoErrno(cerr)
	return newCloseError(CloseResult(resultCodeInt(result)), errno)
}

func bindErrorFromCall[T resultCodeValue](result T, cerr error) error {
	if resultCodeInt(result) == int(BindOK) {
		return nil
	}
	errno := cgoErrno(cerr)
	return newBindError(BindResult(resultCodeInt(result)), errno)
}

func connectErrorFromCall[T resultCodeValue](result T, cerr error) error {
	if resultCodeInt(result) == int(ConnectOK) {
		return nil
	}
	errno := cgoErrno(cerr)
	return newConnectError(ConnectResult(resultCodeInt(result)), errno)
}

func configErrorFromCall[T resultCodeValue](result T, cerr error) error {
	if resultCodeInt(result) == int(ConfigOK) {
		return nil
	}
	errno := cgoErrno(cerr)
	return newConfigError(ConfigResult(resultCodeInt(result)), errno)
}

func validationError(format string, args ...any) error {
	return newConfigError(ConfigInvalidArgument, int(C.EINVAL))
}

func stateError(format string, args ...any) error {
	return newConfigError(ConfigInvalidHandle, int(C.EFAULT))
}

func configErrorFromErrno(errno int) error {
	switch errno {
	case int(C.EFAULT):
		return newConfigError(ConfigInvalidHandle, errno)
	case int(C.EINVAL), int(C.EMSGSIZE):
		return newConfigError(ConfigInvalidArgument, errno)
	case int(C.ENOTSUP):
		return newConfigError(ConfigNotSupported, errno)
	case int(C.EBUSY), int(C.ESHUTDOWN), int(C.ESTALE), int(C.EALREADY),
		int(C.ENOTCONN), int(C.ETIMEDOUT), int(C.EPROTO):
		return newConfigError(ConfigInvalidState, errno)
	case int(C.ENOENT):
		return newConfigError(ConfigNotFound, errno)
	case int(C.EEXIST):
		return newConfigError(ConfigConflict, errno)
	case int(C.ENOBUFS):
		return newConfigError(ConfigBufferTooSmall, errno)
	default:
		return newConfigError(ConfigInternalError, errno)
	}
}

func resultCodeInt[T resultCodeValue](value T) int {
	return int(value)
}
