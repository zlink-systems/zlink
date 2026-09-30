/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.errors;

import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.internal.NativeErrorCodes;
/** Thrown when a request fails or its reply reports an error. */
public final class ZlinkRequestException
  extends TypedZlinkException {
    public ZlinkRequestException(RequestResult result) {
        this(result, representativeErrno(result));
    }

    public ZlinkRequestException(RequestResult result, int nativeErrno) {
        super(result, result.value(), nativeErrno);
    }

    public RequestResult getResult() {
        return (RequestResult) result();
    }

    private static int representativeErrno(RequestResult result) {
        return switch (result) {
            case OK -> 0;
            case TIMED_OUT -> NativeErrorCodes.ETIMEDOUT;
            case NOT_FOUND -> NativeErrorCodes.ENOENT;
            case TERMINATED -> NativeErrorCodes.ETERM;
            case PROTOCOL_ERROR -> NativeErrorCodes.EPROTO;
            case INTERNAL_ERROR -> NativeErrorCodes.EIO;
            case REJECTED -> NativeErrorCodes.EACCES;
            case CONFLICT -> NativeErrorCodes.EEXIST;
            case BUSY -> NativeErrorCodes.EBUSY;
            case NOT_CONNECTED -> NativeErrorCodes.ENOTCONN;
            case INVALID_ARGUMENT -> NativeErrorCodes.EINVAL;
            case INVALID_STATE -> ErrorCode.EFSM.getValue();
            case NOT_SUPPORTED -> NativeErrorCodes.ENOTSUP;
            case BACKPRESSURED -> NativeErrorCodes.EAGAIN;
        };
    }
}
