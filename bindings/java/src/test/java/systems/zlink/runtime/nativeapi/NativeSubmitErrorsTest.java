/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;
import systems.zlink.contracts.errors.ErrorCategory;
import systems.zlink.contracts.errors.HandlerResult;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.errors.ZlinkHandlerException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;

class NativeSubmitErrorsTest {
    @Test
    void preservesEveryCoreSubmitResultBeforeConsultingErrno() {
        for (SubmitResult result : SubmitResult.values()) {
            if (result == SubmitResult.OK) {
                continue;
            }
            ZlinkSubmitException failure = NativeSubmitErrors.submitException(
                result.value(), NativeErrno.EINVAL);
            assertEquals(result, failure.getResult());
            assertEquals(NativeErrno.EINVAL, failure.getNativeErrno());
        }
    }

    @Test
    void mapsNotFoundAndNotConnectedErrnosConsistently() {
        ZlinkSubmitException notFound = (ZlinkSubmitException)
            ZlinkException.fromErrno(ErrorCategory.SUBMIT, NativeErrno.ENOENT);
        assertThrows(IllegalArgumentException.class,
            () -> NativeSubmitErrors.submitException(-1, NativeErrno.ENOENT));
        ZlinkSubmitException notConnected = (ZlinkSubmitException)
            ZlinkException.fromErrno(ErrorCategory.SUBMIT,
                NativeErrno.EHOSTUNREACH);

        assertEquals(SubmitResult.NOT_FOUND, notFound.getResult());
        assertEquals(SubmitResult.NOT_CONNECTED, notConnected.getResult());
    }

    @Test
    void handlerErrnoFallbackUsesCoreCategories() {
        ZlinkHandlerException busy = (ZlinkHandlerException)
            ZlinkException.fromErrno(ErrorCategory.HANDLER, NativeErrno.EBUSY);
        ZlinkHandlerException deadlock = (ZlinkHandlerException)
            ZlinkException.fromErrno(ErrorCategory.HANDLER, NativeErrno.EDEADLK);
        ZlinkHandlerException unknown = (ZlinkHandlerException)
            ZlinkException.fromErrno(ErrorCategory.HANDLER, -1);

        assertEquals(HandlerResult.BUSY, busy.getResult());
        assertEquals(HandlerResult.DEADLOCK, deadlock.getResult());
        assertEquals(HandlerResult.INTERNAL_ERROR, unknown.getResult());
    }
}
