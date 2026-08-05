/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.errors;

import systems.zlink.contracts.sockets.RequestResult;
/** Thrown when a request fails or its reply reports an error. */
public final class ZlinkRequestException
  extends TypedZlinkException {
    public ZlinkRequestException(RequestResult result) {
        this(result, 0);
    }

    public ZlinkRequestException(RequestResult result, int nativeErrno) {
        super(result, result.value(), nativeErrno);
    }

    public RequestResult getResult() {
        return (RequestResult) result();
    }
}
