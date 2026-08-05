/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import java.util.function.IntPredicate;
import java.util.function.IntSupplier;
import systems.zlink.internal.NativeErrorCodes;

public final class NativeErrno {
    public static final int ENOENT = NativeErrorCodes.ENOENT;
    public static final int EINTR = NativeErrorCodes.EINTR;
    public static final int EBADF = NativeErrorCodes.EBADF;
    public static final int EAGAIN = NativeErrorCodes.EAGAIN;
    public static final int ENOMEM = NativeErrorCodes.ENOMEM;
    public static final int EFAULT = NativeErrorCodes.EFAULT;
    public static final int EBUSY = NativeErrorCodes.EBUSY;
    public static final int EINVAL = NativeErrorCodes.EINVAL;
    public static final int EADDRINUSE = NativeErrorCodes.EADDRINUSE;
    public static final int ECONNREFUSED = NativeErrorCodes.ECONNREFUSED;
    public static final int ENOTSUP = NativeErrorCodes.ENOTSUP;
    public static final int ENOTCONN = NativeErrorCodes.ENOTCONN;
    public static final int EHOSTUNREACH = NativeErrorCodes.EHOSTUNREACH;
    public static final int EWOULDBLOCK_WIN = NativeErrorCodes.EWOULDBLOCK_WIN;
    public static final int ECONNREFUSED_WIN = NativeErrorCodes.ECONNREFUSED_WIN;
    public static final int ENOTCONN_WIN = NativeErrorCodes.ENOTCONN_WIN;
    public static final int EHOSTUNREACH_WIN = NativeErrorCodes.EHOSTUNREACH_WIN;

    private NativeErrno() {
    }

    public static int retryWhileInterrupted(IntSupplier call,
                                            IntPredicate failed) {
        while (true) {
            int rc = call.getAsInt();
            if (!failed.test(rc) || Native.errno() != EINTR) {
                return rc;
            }
        }
    }
}
