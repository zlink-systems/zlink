/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;

public final class NativeSubmitErrors {
    private NativeSubmitErrors() {
    }

    public static boolean isBackpressured(int errno) {
        return errno == NativeErrno.EAGAIN
            || errno == NativeErrno.EWOULDBLOCK_WIN;
    }

    public static ZlinkSubmitException submitException(int result, int errno) {
        if (result == SubmitResult.OK.value()) {
            throw new IllegalArgumentException(
                "submit result indicates success");
        }
        return new ZlinkSubmitException(SubmitResult.fromValue(result), errno);
    }
}
