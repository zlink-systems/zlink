/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.errors;


/** Thrown when binding a socket to an endpoint fails. */
public final class ZlinkBindException
  extends TypedZlinkException {
    public ZlinkBindException(BindResult result) {
        this(result, 0);
    }

    public ZlinkBindException(BindResult result, int nativeErrno) {
        super(result, result.value(), nativeErrno);
    }

    public BindResult getResult() {
        return (BindResult) result();
    }
}
