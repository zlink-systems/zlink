/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf;

public final class PerfErrno {
    public static final int EINTR = 4;
    public static final int EAGAIN = 11;
    public static final int ENOTCONN = 107;
    public static final int ETIMEDOUT = 110;
    public static final int EHOSTUNREACH = 113;
    public static final int EWOULDBLOCK_WIN = 10035;

    private PerfErrno() {
    }

    public static boolean isRetryableSend(int errno) {
        return errno == EAGAIN
            || errno == EINTR
            || errno == EWOULDBLOCK_WIN;
    }
}
