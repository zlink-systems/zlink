/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.errors;


/** Thrown when connecting a socket to an endpoint fails. */
public final class ZlinkConnectException
  extends TypedZlinkException {
    public ZlinkConnectException(ConnectResult result) {
        this(result, 0);
    }

    public ZlinkConnectException(ConnectResult result, int nativeErrno) {
        super(result, result.value(), nativeErrno);
    }

    public ConnectResult getResult() {
        return (ConnectResult) result();
    }
}
