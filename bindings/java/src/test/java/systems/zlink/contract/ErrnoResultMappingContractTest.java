/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.errors.CloseResult;
import systems.zlink.contracts.errors.ErrorCategory;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.RequestResult;
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

    @Test
    void requestErrnoFollowsCoreTable() {
        assertRequest(RequestResult.TIMED_OUT, NativeErrorCodes.ETIMEDOUT);
        assertRequest(RequestResult.NOT_FOUND, NativeErrorCodes.ENOENT);
        assertRequest(RequestResult.TERMINATED, NativeErrorCodes.ETERM);
        assertRequest(RequestResult.TERMINATED, NativeErrorCodes.ESHUTDOWN);
        assertRequest(RequestResult.PROTOCOL_ERROR, NativeErrorCodes.EPROTO);
        assertRequest(RequestResult.PROTOCOL_ERROR, NativeErrorCodes.ENOCOMPATPROTO);
        assertRequest(RequestResult.INTERNAL_ERROR, NativeErrorCodes.EIO);
        assertRequest(RequestResult.INTERNAL_ERROR, NativeErrorCodes.EINTR);
        assertRequest(RequestResult.REJECTED, NativeErrorCodes.EACCES);
        assertRequest(RequestResult.REJECTED, NativeErrorCodes.ECONNREFUSED);
        assertRequest(RequestResult.REJECTED, NativeErrorCodes.ECANCELED);
        assertRequest(RequestResult.REJECTED, NativeErrorCodes.EPROTOTYPE);
        assertRequest(RequestResult.CONFLICT, NativeErrorCodes.EEXIST);
        assertRequest(RequestResult.CONFLICT, NativeErrorCodes.ESTALE);
        assertRequest(RequestResult.BUSY, NativeErrorCodes.EBUSY);
        assertRequest(RequestResult.NOT_CONNECTED, NativeErrorCodes.ENOTCONN);
        assertRequest(RequestResult.NOT_CONNECTED, NativeErrorCodes.EHOSTUNREACH);
        assertRequest(RequestResult.INVALID_ARGUMENT, NativeErrorCodes.EINVAL);
        assertRequest(RequestResult.INVALID_ARGUMENT, NativeErrorCodes.EFAULT);
        assertRequest(RequestResult.INVALID_STATE, NativeErrorCodes.EFSM);
        assertRequest(RequestResult.INVALID_STATE, NativeErrorCodes.EALREADY);
        assertRequest(RequestResult.NOT_SUPPORTED, NativeErrorCodes.ENOTSUP);
        assertRequest(RequestResult.NOT_SUPPORTED, NativeErrorCodes.EOPNOTSUPP);
        assertRequest(RequestResult.BACKPRESSURED, NativeErrorCodes.EAGAIN);
        assertRequest(RequestResult.BACKPRESSURED, NativeErrorCodes.ENOBUFS);
        assertRequest(RequestResult.INTERNAL_ERROR, -1);
    }

    private static void assertRequest(RequestResult expected, int errno) {
        ZlinkException error = ZlinkException.fromErrno(ErrorCategory.REQUEST, errno);
        assertEquals(expected, ((ZlinkRequestException) error).getResult(), "errno " + errno);
        assertEquals(errno, error.getNativeErrno());
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
