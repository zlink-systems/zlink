/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.errors;


/** Thrown when registering or running a callback handler fails. */
public final class ZlinkHandlerException
  extends TypedZlinkException {
    public ZlinkHandlerException(HandlerResult result) {
        this(result, 0);
    }

    public ZlinkHandlerException(HandlerResult result, int nativeErrno) {
        super(result, result.value(), nativeErrno);
    }

    public HandlerResult getResult() {
        return (HandlerResult) result();
    }
}
