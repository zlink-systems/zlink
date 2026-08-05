/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.errors;

abstract sealed class TypedZlinkException extends ZlinkException
  permits ZlinkBindException, ZlinkCloseException, ZlinkConfigException,
          ZlinkConnectException, ZlinkHandlerException, ZlinkRecvException,
          ZlinkRequestException, ZlinkSubmitException {
    private final Object result;

    TypedZlinkException(Object result, int code, int nativeErrno) {
        this(result, null, code, nativeErrno);
    }

    TypedZlinkException(Object result, String message, int code,
                        int nativeErrno) {
        super(message, code, nativeErrno);
        this.result = result;
    }

    protected final Object result() {
        return result;
    }
}
