/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.errors;

import systems.zlink.contracts.sockets.SubmitResult;
/** Thrown when submitting a send or publish fails. */
public final class ZlinkSubmitException
  extends TypedZlinkException {
    public ZlinkSubmitException(SubmitResult result) {
        this(result, 0);
    }

    public ZlinkSubmitException(SubmitResult result, int nativeErrno) {
        super(result, result.value(), nativeErrno);
    }

    public SubmitResult getResult() {
        return (SubmitResult) result();
    }
}
