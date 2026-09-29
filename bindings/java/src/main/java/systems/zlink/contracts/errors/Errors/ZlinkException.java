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
        return switch (errno) {
            case NativeErrorCodes.EFAULT, NativeErrorCodes.EBADF ->
                SubmitResult.INVALID_HANDLE;
            case NativeErrorCodes.EAGAIN, NativeErrorCodes.EWOULDBLOCK_WIN ->
                SubmitResult.BACKPRESSURED;
            case NativeErrorCodes.ENOTCONN, NativeErrorCodes.ENOTCONN_WIN,
                 NativeErrorCodes.EHOSTUNREACH,
                 NativeErrorCodes.EHOSTUNREACH_WIN ->
                SubmitResult.NOT_CONNECTED;
            case NativeErrorCodes.ENOENT ->
                SubmitResult.NOT_FOUND;
            case NativeErrorCodes.ECONNREFUSED, NativeErrorCodes.ECONNREFUSED_WIN ->
                SubmitResult.NOT_ADMITTED;
            case NativeErrorCodes.ECANCELED, NativeErrorCodes.ESHUTDOWN,
                 NativeErrorCodes.ETERM -> SubmitResult.TERMINATED;
            case NativeErrorCodes.EINVAL -> SubmitResult.INVALID_ARGUMENT;
            case NativeErrorCodes.ENOTSUP -> SubmitResult.NOT_SUPPORTED;
            case NativeErrorCodes.ENOMEM -> SubmitResult.OUT_OF_MEMORY;
            default -> SubmitResult.INTERNAL_ERROR;
        };
    }

    private static RecvResult mapRecvResult(int errno) {
        return switch (errno) {
            case NativeErrorCodes.EFAULT -> RecvResult.INVALID_HANDLE;
            case NativeErrorCodes.EAGAIN, NativeErrorCodes.EWOULDBLOCK_WIN,
                 NativeErrorCodes.ETIMEDOUT -> RecvResult.NO_DATA;
            case NativeErrorCodes.EBUSY -> RecvResult.BUSY;
            case NativeErrorCodes.ETERM -> RecvResult.TERMINATED;
            case NativeErrorCodes.ENOTSUP -> RecvResult.NOT_SUPPORTED;
            case NativeErrorCodes.ENOBUFS -> RecvResult.BUFFER_TOO_SMALL;
            case NativeErrorCodes.EINVAL, NativeErrorCodes.ESTALE,
                 NativeErrorCodes.ESHUTDOWN -> RecvResult.INVALID_STATE;
            default -> RecvResult.INTERNAL_ERROR;
        };
    }

    private static BindResult mapBindResult(int errno) {
        return switch (errno) {
            case NativeErrorCodes.EFAULT, NativeErrorCodes.EBADF ->
                BindResult.INVALID_HANDLE;
            case NativeErrorCodes.EINVAL -> BindResult.INVALID_ARGUMENT;
            case NativeErrorCodes.EADDRINUSE -> BindResult.ADDR_IN_USE;
            case NativeErrorCodes.ENOTSUP -> BindResult.NOT_SUPPORTED;
            default -> BindResult.INVALID_ARGUMENT;
        };
    }

    private static ConnectResult mapConnectResult(int errno) {
        return switch (errno) {
            case NativeErrorCodes.EFAULT, NativeErrorCodes.EBADF ->
                ConnectResult.INVALID_HANDLE;
            case NativeErrorCodes.EINVAL -> ConnectResult.INVALID_ARGUMENT;
            case NativeErrorCodes.ENOTSUP -> ConnectResult.NOT_SUPPORTED;
            case NativeErrorCodes.EBUSY -> ConnectResult.BUSY;
            default -> ConnectResult.INVALID_ARGUMENT;
        };
    }

    private static CloseResult mapCloseResult(int errno) {
        return switch (errno) {
            case NativeErrorCodes.EFAULT, NativeErrorCodes.ESTALE ->
                CloseResult.INVALID_HANDLE;
            case NativeErrorCodes.EBUSY, NativeErrorCodes.EDEADLK -> CloseResult.BUSY;
            case NativeErrorCodes.ESHUTDOWN -> CloseResult.SHUTDOWN;
            default -> CloseResult.INTERNAL_ERROR;
        };
    }

    private static HandlerResult mapHandlerResult(int errno) {
        return switch (errno) {
            case NativeErrorCodes.EFAULT, NativeErrorCodes.EBADF ->
                HandlerResult.INVALID_HANDLE;
            case NativeErrorCodes.EINVAL -> HandlerResult.INVALID_ARGUMENT;
            case NativeErrorCodes.EBUSY -> HandlerResult.BUSY;
            case NativeErrorCodes.ENOTSUP -> HandlerResult.NOT_SUPPORTED;
            case NativeErrorCodes.EDEADLK -> HandlerResult.DEADLOCK;
            default -> HandlerResult.INTERNAL_ERROR;
        };
    }

    private static ConfigResult mapConfigResult(int errno) {
        return switch (errno) {
            case NativeErrorCodes.EFAULT, NativeErrorCodes.EBADF ->
                ConfigResult.INVALID_HANDLE;
            case NativeErrorCodes.EINVAL -> ConfigResult.INVALID_ARGUMENT;
            case NativeErrorCodes.ENOTSUP -> ConfigResult.NOT_SUPPORTED;
            case NativeErrorCodes.EBUSY, NativeErrorCodes.ESHUTDOWN -> ConfigResult.INVALID_STATE;
            default -> ConfigResult.INTERNAL_ERROR;
        };
    }

    private static RequestResult mapRequestResult(int errno) {
        return switch (errno) {
            case NativeErrorCodes.EAGAIN, NativeErrorCodes.EWOULDBLOCK_WIN ->
                RequestResult.TIMED_OUT;
            case NativeErrorCodes.ENOTCONN, NativeErrorCodes.ENOTCONN_WIN,
                 NativeErrorCodes.EHOSTUNREACH, NativeErrorCodes.EHOSTUNREACH_WIN,
                 NativeErrorCodes.ENOENT ->
                RequestResult.NOT_FOUND;
            case NativeErrorCodes.ECANCELED, NativeErrorCodes.ESHUTDOWN,
                 NativeErrorCodes.ETERM -> RequestResult.TERMINATED;
            case NativeErrorCodes.EINTR -> RequestResult.PROTOCOL_ERROR;
            default -> RequestResult.PROTOCOL_ERROR;
        };
    }
}
