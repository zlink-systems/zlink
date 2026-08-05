/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.errors;

import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.internal.ContractAccess;
import systems.zlink.internal.NativeErrorCodes;
import java.util.Locale;

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

    public static ZlinkException fromLastError(String operation) {
        return fromLastError(categoryFromOperation(operation));
    }

    public static ZlinkException fromLastError(ErrorCategory category) {
        return fromErrno(category, safeErrno());
    }

    public static ZlinkException fromErrno(String operation, int errno) {
        return fromErrno(categoryFromOperation(operation), errno);
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

    private static ErrorCategory categoryFromOperation(String operation) {
        String op = operation == null ? "" : operation.toLowerCase(Locale.ROOT);
        if (containsAny(op, "handler")) {
            return ErrorCategory.HANDLER;
        }
        if (containsAny(op, "recv", "receive", "subscription_event",
                "socket_monitor_recv", "monitor_recv")
            || (op.contains("subscribe")
                && !containsAny(op, "set_subscription", "unset_subscription",
                    "subscribe_handler"))) {
            return ErrorCategory.RECV;
        }
        if (containsAny(op, "request")) {
            return ErrorCategory.REQUEST;
        }
        if (containsAny(op, "bind")) {
            return ErrorCategory.BIND;
        }
        if (containsAny(op, "connect", "disconnect", "unbind")) {
            return ErrorCategory.CONNECT;
        }
        if (containsAny(op, "close", "destroy")) {
            return ErrorCategory.CLOSE;
        }
        if (containsAny(op, "send", "publish", "reply", "proxy")) {
            return ErrorCategory.SUBMIT;
        }
        return ErrorCategory.CONFIG;
    }

    private static int safeErrno() {
        try {
            return ContractAccess.nativeErrno();
        } catch (RuntimeException ignored) {
            return -1;
        }
    }

    private static boolean containsAny(String value, String... needles) {
        for (String needle : needles) {
            if (value.contains(needle)) {
                return true;
            }
        }
        return false;
    }

    private static SubmitResult mapSubmitResult(int errno) {
        return switch (errno) {
            case NativeErrorCodes.EFAULT, NativeErrorCodes.EBADF ->
                SubmitResult.INVALID_HANDLE;
            case NativeErrorCodes.EAGAIN, NativeErrorCodes.EWOULDBLOCK_WIN ->
                SubmitResult.BACKPRESSURED;
            case NativeErrorCodes.ENOTCONN, NativeErrorCodes.ENOTCONN_WIN ->
                SubmitResult.NOT_CONNECTED;
            case NativeErrorCodes.EHOSTUNREACH, NativeErrorCodes.EHOSTUNREACH_WIN ->
                SubmitResult.NOT_FOUND;
            case NativeErrorCodes.ECONNREFUSED, NativeErrorCodes.ECONNREFUSED_WIN ->
                SubmitResult.NOT_ADMITTED;
            case NativeErrorCodes.EINVAL -> SubmitResult.INVALID_ARGUMENT;
            case NativeErrorCodes.ENOTSUP -> SubmitResult.NOT_SUPPORTED;
            case NativeErrorCodes.ENOMEM -> SubmitResult.OUT_OF_MEMORY;
            default -> SubmitResult.INTERNAL_ERROR;
        };
    }

    private static RecvResult mapRecvResult(int errno) {
        return switch (errno) {
            case NativeErrorCodes.EFAULT, NativeErrorCodes.EBADF ->
                RecvResult.INVALID_HANDLE;
            case NativeErrorCodes.EAGAIN, NativeErrorCodes.EWOULDBLOCK_WIN ->
                RecvResult.NO_DATA;
            case NativeErrorCodes.EINTR -> RecvResult.BUSY;
            case NativeErrorCodes.ENOTCONN, NativeErrorCodes.ENOTCONN_WIN ->
                RecvResult.TERMINATED;
            case NativeErrorCodes.ENOTSUP -> RecvResult.NOT_SUPPORTED;
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
            case NativeErrorCodes.EFAULT, NativeErrorCodes.EBADF ->
                CloseResult.INVALID_HANDLE;
            case NativeErrorCodes.EAGAIN, NativeErrorCodes.EWOULDBLOCK_WIN ->
                CloseResult.BUSY;
            case NativeErrorCodes.ENOTCONN, NativeErrorCodes.ENOTCONN_WIN ->
                CloseResult.SHUTDOWN;
            default -> CloseResult.BUSY;
        };
    }

    private static HandlerResult mapHandlerResult(int errno) {
        return switch (errno) {
            case NativeErrorCodes.EFAULT, NativeErrorCodes.EBADF ->
                HandlerResult.INVALID_HANDLE;
            case NativeErrorCodes.EINVAL -> HandlerResult.INVALID_ARGUMENT;
            case NativeErrorCodes.EAGAIN, NativeErrorCodes.EWOULDBLOCK_WIN ->
                HandlerResult.BUSY;
            case NativeErrorCodes.ENOTSUP -> HandlerResult.NOT_SUPPORTED;
            case NativeErrorCodes.EBUSY -> HandlerResult.DEADLOCK;
            default -> HandlerResult.BUSY;
        };
    }

    private static ConfigResult mapConfigResult(int errno) {
        return switch (errno) {
            case NativeErrorCodes.EFAULT, NativeErrorCodes.EBADF ->
                ConfigResult.INVALID_HANDLE;
            case NativeErrorCodes.EINVAL -> ConfigResult.INVALID_ARGUMENT;
            case NativeErrorCodes.ENOTSUP -> ConfigResult.NOT_SUPPORTED;
            default -> ConfigResult.INTERNAL_ERROR;
        };
    }

    private static RequestResult mapRequestResult(int errno) {
        return switch (errno) {
            case NativeErrorCodes.EAGAIN, NativeErrorCodes.EWOULDBLOCK_WIN ->
                RequestResult.TIMED_OUT;
            case NativeErrorCodes.ENOTCONN, NativeErrorCodes.ENOTCONN_WIN,
                 NativeErrorCodes.EHOSTUNREACH, NativeErrorCodes.EHOSTUNREACH_WIN ->
                RequestResult.NOT_FOUND;
            case NativeErrorCodes.EINTR -> RequestResult.PROTOCOL_ERROR;
            default -> RequestResult.PROTOCOL_ERROR;
        };
    }
}
