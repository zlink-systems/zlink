/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;
import systems.zlink.contracts.errors.CloseResult;
import systems.zlink.contracts.errors.ErrorCategory;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.internal.NativeErrorCodes;

/**
 * core/doc/spec/core/03-errors.ko.md "Result와 errno 대응": the receive (§4) and close (§5) rows.
 * An errno the table does not list maps to the internal-error result.
 */
class ErrnoResultMappingContractTest {
    @Test
    void receiveErrnoFollowsCoreTable() {
        assertRecv(RecvResult.INVALID_HANDLE, NativeErrorCodes.EFAULT);
        assertRecv(RecvResult.NO_DATA, NativeErrorCodes.EAGAIN);
        assertRecv(RecvResult.NO_DATA, NativeErrorCodes.ETIMEDOUT);
        assertRecv(RecvResult.BUSY, NativeErrorCodes.EBUSY);
        assertRecv(RecvResult.TERMINATED, NativeErrorCodes.ETERM);
        assertRecv(RecvResult.NOT_SUPPORTED, NativeErrorCodes.ENOTSUP);
        assertRecv(RecvResult.BUFFER_TOO_SMALL, NativeErrorCodes.ENOBUFS);
        assertRecv(RecvResult.INVALID_STATE, NativeErrorCodes.EINVAL);
        assertRecv(RecvResult.INVALID_STATE, NativeErrorCodes.ESTALE);
        assertRecv(RecvResult.INVALID_STATE, NativeErrorCodes.ESHUTDOWN);
        assertRecv(RecvResult.INTERNAL_ERROR, NativeErrorCodes.EINTR);
    }

    @Test
    void closeErrnoFollowsCoreTable() {
        assertClose(CloseResult.BUSY, NativeErrorCodes.EBUSY);
        assertClose(CloseResult.BUSY, NativeErrorCodes.EDEADLK);
        assertClose(CloseResult.SHUTDOWN, NativeErrorCodes.ESHUTDOWN);
        assertClose(CloseResult.INVALID_HANDLE, NativeErrorCodes.EFAULT);
        assertClose(CloseResult.INVALID_HANDLE, NativeErrorCodes.ESTALE);
        assertClose(CloseResult.INTERNAL_ERROR, NativeErrorCodes.EAGAIN);
    }

    private static void assertRecv(RecvResult expected, int errno) {
        ZlinkException error = ZlinkException.fromErrno(ErrorCategory.RECV, errno);
        assertEquals(expected, ((ZlinkRecvException) error).getResult(), "errno " + errno);
        assertEquals(errno, error.getNativeErrno());
    }

    private static void assertClose(CloseResult expected, int errno) {
        ZlinkException error = ZlinkException.fromErrno(ErrorCategory.CLOSE, errno);
        assertEquals(expected, ((ZlinkCloseException) error).getResult(), "errno " + errno);
        assertEquals(errno, error.getNativeErrno());
    }
}
