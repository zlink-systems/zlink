/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import systems.zlink.internal.NativeErrorCodes;

public final class NativeErrno {
    public static final int ENOENT = NativeErrorCodes.ENOENT;
    public static final int EINTR = NativeErrorCodes.EINTR;
    public static final int EBADF = NativeErrorCodes.EBADF;
    public static final int EAGAIN = NativeErrorCodes.EAGAIN;
    public static final int EDEADLK = NativeErrorCodes.EDEADLK;
    public static final int ENOMEM = NativeErrorCodes.ENOMEM;
    public static final int EFAULT = NativeErrorCodes.EFAULT;
    public static final int EBUSY = NativeErrorCodes.EBUSY;
    public static final int EINVAL = NativeErrorCodes.EINVAL;
    public static final int EPROTO = NativeErrorCodes.EPROTO;
    public static final int EADDRINUSE = NativeErrorCodes.EADDRINUSE;
    public static final int ECONNREFUSED = NativeErrorCodes.ECONNREFUSED;
    public static final int ENOTSUP = NativeErrorCodes.ENOTSUP;
    public static final int ENOTCONN = NativeErrorCodes.ENOTCONN;
    public static final int EHOSTUNREACH = NativeErrorCodes.EHOSTUNREACH;
    public static final int ESHUTDOWN = NativeErrorCodes.ESHUTDOWN;
    public static final int ETIMEDOUT = NativeErrorCodes.ETIMEDOUT;
    public static final int ECANCELED = NativeErrorCodes.ECANCELED;
    public static final int ETERM = NativeErrorCodes.ETERM;

    private NativeErrno() {}
}
