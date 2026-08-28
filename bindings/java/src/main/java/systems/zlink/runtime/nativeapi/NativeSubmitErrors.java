/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import systems.zlink.contracts.errors.ErrorCategory;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;

public final class NativeSubmitErrors {
    private NativeSubmitErrors() {
    }

    public static boolean isBackpressured(int errno) {
        return errno == NativeErrno.EAGAIN
            || errno == NativeErrno.EWOULDBLOCK_WIN;
    }

    public static boolean isNotConnected(int errno) {
        return errno == NativeErrno.ENOTCONN
            || errno == NativeErrno.ENOTCONN_WIN
            || errno == NativeErrno.EHOSTUNREACH
            || errno == NativeErrno.EHOSTUNREACH_WIN;
    }

    public static boolean isNotFound(int errno) {
        return errno == NativeErrno.ENOENT;
    }

    public static boolean isNotAdmitted(int errno) {
        return errno == NativeErrno.ECONNREFUSED
            || errno == NativeErrno.ECONNREFUSED_WIN;
    }

    public static ZlinkSubmitException submitExceptionOrNull(int errno) {
        if (isBackpressured(errno)) {
            return new ZlinkSubmitException(SubmitResult.BACKPRESSURED, errno);
        }
        if (isNotConnected(errno)) {
            return new ZlinkSubmitException(SubmitResult.NOT_CONNECTED, errno);
        }
        if (isNotFound(errno)) {
            return new ZlinkSubmitException(SubmitResult.NOT_FOUND, errno);
        }
        if (isNotAdmitted(errno)) {
            return new ZlinkSubmitException(SubmitResult.NOT_ADMITTED, errno);
        }
        return null;
    }

    public static ZlinkSubmitException submitException(int result, int errno) {
        if (result == SubmitResult.OK.value()) {
            throw new IllegalArgumentException(
                "submit result indicates success");
        }
        try {
            return new ZlinkSubmitException(SubmitResult.fromValue(result),
                errno);
        } catch (IllegalArgumentException ignored) {
            ZlinkSubmitException mapped = submitExceptionOrNull(errno);
            if (mapped != null) {
                return mapped;
            }
            return (ZlinkSubmitException) ZlinkException.fromErrno(
                ErrorCategory.SUBMIT, errno);
        }
    }
}
