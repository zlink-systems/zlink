/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.errors;

import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.internal.ContractAccess;
import systems.zlink.internal.NativeErrorCodes;

/** Base class for all exceptions thrown by the zlink bindings. */
public abstract sealed class ZlinkException extends RuntimeException
  permits TypedZlinkException {
    private final int code;
    private final int nativeErrno;

    protected ZlinkException(int code) {
        this(code, 0);
    }

    protected ZlinkException(int code, int nativeErrno) {
        this(null, code, nativeErrno);
    }

    protected ZlinkException(String message, int code, int nativeErrno) {
        super(message);
        this.code = code;
        this.nativeErrno = nativeErrno;
    }

    public int getCode() {
        return code;
    }

    public int getNativeErrno() {
        return nativeErrno;
    }

    public static ZlinkException fromLastError(ErrorCategory category) {
        return fromErrno(category, ContractAccess.nativeErrno());
    }

    public static ZlinkException fromErrno(ErrorCategory category, int errno) {
        return switch (category) {
            case HANDLER -> new ZlinkHandlerException(mapHandlerResult(errno),
                errno);
            case RECV -> new ZlinkRecvException(mapRecvResult(errno), errno);
            case REQUEST -> new ZlinkRequestException(mapRequestResult(errno),
                errno);
            case BIND -> new ZlinkBindException(mapBindResult(errno), errno);
            case CONNECT -> new ZlinkConnectException(mapConnectResult(errno),
                errno);
            case CLOSE -> new ZlinkCloseException(mapCloseResult(errno),
                errno);
            case SUBMIT -> new ZlinkSubmitException(mapSubmitResult(errno),
                errno);
            case CONFIG -> new ZlinkConfigException(mapConfigResult(errno),
                errno);
        };
    }

    private static SubmitResult mapSubmitResult(int errno) {
        if (errno == NativeErrorCodes.EFAULT || errno == NativeErrorCodes.EBADF) {
            return SubmitResult.INVALID_HANDLE;
        }
        if (errno == NativeErrorCodes.EAGAIN) {
            return SubmitResult.BACKPRESSURED;
        }
        if (errno == NativeErrorCodes.ENOTCONN || errno == NativeErrorCodes.EHOSTUNREACH) {
            return SubmitResult.NOT_CONNECTED;
        }
        if (errno == NativeErrorCodes.ENOENT) {
            return SubmitResult.NOT_FOUND;
        }
        if (errno == NativeErrorCodes.ECONNREFUSED) {
            return SubmitResult.NOT_ADMITTED;
        }
        if (errno == NativeErrorCodes.ECANCELED
                || errno == NativeErrorCodes.ESHUTDOWN
                || errno == NativeErrorCodes.ETERM) {
            return SubmitResult.TERMINATED;
        }
        if (errno == NativeErrorCodes.EINVAL) {
            return SubmitResult.INVALID_ARGUMENT;
        }
        if (errno == NativeErrorCodes.ENOTSUP) {
            return SubmitResult.NOT_SUPPORTED;
        }
        if (errno == NativeErrorCodes.ENOMEM) {
            return SubmitResult.OUT_OF_MEMORY;
        }
        return SubmitResult.INTERNAL_ERROR;
    }

    private static RecvResult mapRecvResult(int errno) {
        if (errno == NativeErrorCodes.EFAULT) {
            return RecvResult.INVALID_HANDLE;
        }
        if (errno == NativeErrorCodes.EAGAIN || errno == NativeErrorCodes.ETIMEDOUT) {
            return RecvResult.NO_DATA;
        }
        if (errno == NativeErrorCodes.EBUSY) {
            return RecvResult.BUSY;
        }
        if (errno == NativeErrorCodes.ETERM) {
            return RecvResult.TERMINATED;
        }
        if (errno == NativeErrorCodes.ENOTSUP) {
            return RecvResult.NOT_SUPPORTED;
        }
        if (errno == NativeErrorCodes.ENOBUFS) {
            return RecvResult.BUFFER_TOO_SMALL;
        }
        if (errno == NativeErrorCodes.EINVAL
                || errno == NativeErrorCodes.ESTALE
                || errno == NativeErrorCodes.ESHUTDOWN) {
            return RecvResult.INVALID_STATE;
        }
        return RecvResult.INTERNAL_ERROR;
    }

    private static BindResult mapBindResult(int errno) {
        if (errno == NativeErrorCodes.EFAULT || errno == NativeErrorCodes.EBADF) {
            return BindResult.INVALID_HANDLE;
        }
        if (errno == NativeErrorCodes.EINVAL) {
            return BindResult.INVALID_ARGUMENT;
        }
        if (errno == NativeErrorCodes.EADDRINUSE) {
            return BindResult.ADDR_IN_USE;
        }
        if (errno == NativeErrorCodes.ENOTSUP) {
            return BindResult.NOT_SUPPORTED;
        }
        return BindResult.INVALID_ARGUMENT;
    }

    private static ConnectResult mapConnectResult(int errno) {
        if (errno == NativeErrorCodes.EFAULT || errno == NativeErrorCodes.EBADF) {
            return ConnectResult.INVALID_HANDLE;
        }
        if (errno == NativeErrorCodes.EINVAL) {
            return ConnectResult.INVALID_ARGUMENT;
        }
        if (errno == NativeErrorCodes.ENOTSUP) {
            return ConnectResult.NOT_SUPPORTED;
        }
        if (errno == NativeErrorCodes.EBUSY) {
            return ConnectResult.BUSY;
        }
        return ConnectResult.INVALID_ARGUMENT;
    }

    private static CloseResult mapCloseResult(int errno) {
        if (errno == NativeErrorCodes.EFAULT || errno == NativeErrorCodes.ESTALE) {
            return CloseResult.INVALID_HANDLE;
        }
        if (errno == NativeErrorCodes.EBUSY || errno == NativeErrorCodes.EDEADLK) {
            return CloseResult.BUSY;
        }
        if (errno == NativeErrorCodes.ESHUTDOWN) {
            return CloseResult.SHUTDOWN;
        }
        return CloseResult.INTERNAL_ERROR;
    }

    private static HandlerResult mapHandlerResult(int errno) {
        if (errno == NativeErrorCodes.EFAULT || errno == NativeErrorCodes.EBADF) {
            return HandlerResult.INVALID_HANDLE;
        }
        if (errno == NativeErrorCodes.EINVAL) {
            return HandlerResult.INVALID_ARGUMENT;
        }
        if (errno == NativeErrorCodes.EBUSY) {
            return HandlerResult.BUSY;
        }
        if (errno == NativeErrorCodes.ENOTSUP) {
            return HandlerResult.NOT_SUPPORTED;
        }
        if (errno == NativeErrorCodes.EDEADLK) {
            return HandlerResult.DEADLOCK;
        }
        return HandlerResult.INTERNAL_ERROR;
    }

    private static ConfigResult mapConfigResult(int errno) {
        if (errno == NativeErrorCodes.EFAULT || errno == NativeErrorCodes.EBADF) {
            return ConfigResult.INVALID_HANDLE;
        }
        if (errno == NativeErrorCodes.EINVAL) {
            return ConfigResult.INVALID_ARGUMENT;
        }
        if (errno == NativeErrorCodes.ENOTSUP) {
            return ConfigResult.NOT_SUPPORTED;
        }
        if (errno == NativeErrorCodes.EBUSY || errno == NativeErrorCodes.ESHUTDOWN) {
            return ConfigResult.INVALID_STATE;
        }
        return ConfigResult.INTERNAL_ERROR;
    }

    private static RequestResult mapRequestResult(int errno) {
        if (errno == NativeErrorCodes.ETIMEDOUT) return RequestResult.TIMED_OUT;
        if (errno == NativeErrorCodes.ENOENT) return RequestResult.NOT_FOUND;
        if (errno == NativeErrorCodes.ETERM || errno == NativeErrorCodes.ESHUTDOWN)
            return RequestResult.TERMINATED;
        if (errno == NativeErrorCodes.EPROTO || errno == NativeErrorCodes.ENOCOMPATPROTO)
            return RequestResult.PROTOCOL_ERROR;
        if (errno == NativeErrorCodes.EACCES
                || errno == NativeErrorCodes.ECONNREFUSED
                || errno == NativeErrorCodes.ECANCELED
                || errno == NativeErrorCodes.EPROTOTYPE) return RequestResult.REJECTED;
        if (errno == NativeErrorCodes.EEXIST || errno == NativeErrorCodes.ESTALE)
            return RequestResult.CONFLICT;
        if (errno == NativeErrorCodes.EBUSY) return RequestResult.BUSY;
        if (errno == NativeErrorCodes.ENOTCONN || errno == NativeErrorCodes.EHOSTUNREACH)
            return RequestResult.NOT_CONNECTED;
        if (errno == NativeErrorCodes.EINVAL || errno == NativeErrorCodes.EFAULT)
            return RequestResult.INVALID_ARGUMENT;
        if (errno == NativeErrorCodes.EFSM || errno == NativeErrorCodes.EALREADY)
            return RequestResult.INVALID_STATE;
        if (errno == NativeErrorCodes.ENOTSUP || errno == NativeErrorCodes.EOPNOTSUPP)
            return RequestResult.NOT_SUPPORTED;
        if (errno == NativeErrorCodes.EAGAIN || errno == NativeErrorCodes.ENOBUFS)
            return RequestResult.BACKPRESSURED;
        return RequestResult.INTERNAL_ERROR;
    }
}
