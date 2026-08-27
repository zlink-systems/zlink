/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTimeoutPreemptively;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.List;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendResult;
import systems.zlink.contracts.sockets.SocketType;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.internal.sockets.SocketOptions;
import systems.zlink.runtime.nativeapi.NativeErrno;

class SocketSendOwnershipContractTest {
    @Test
    void blockingDirectFailureDoesNotRetryConsumedPart() {
        TestSupport.assumeNative();

        assertTimeoutPreemptively(Duration.ofSeconds(2), () -> {
            try (Context context = Zlink.createContext();
                 NativeSocketRuntime router = new NativeSocketRuntime(context,
                     SocketType.ROUTER);
                 Message part = Message.from("attempted")) {
                router.setOption(SocketOptions.ROUTER_MANDATORY, 1);

                ZlinkSubmitException failure = assertThrows(
                    ZlinkSubmitException.class,
                    () -> router.send(RoutingId.from("missing-route"), part,
                        SendFlag.NONE));

                assertEquals(SubmitResult.NOT_CONNECTED,
                    failure.getResult());
                assertEquals(0, part.size());
            }
        });
    }

    @Test
    void failedDirectSinglePartIsConsumed() {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             NativeSocketRuntime router = new NativeSocketRuntime(context,
                 SocketType.ROUTER);
             Message part = Message.from("attempted")) {
            router.setOption(SocketOptions.ROUTER_MANDATORY, 1);

            SendResult result = router.sendMessageFrameNoWaitResult(
                RoutingId.from("missing-route"), part);

            assertEquals(SendResult.NOT_READY, result);
            assertEquals(0, part.size());
        }
    }

    @Test
    void failedDirectMultipartConsumesOnlyAttemptedPart() {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             NativeSocketRuntime router = new NativeSocketRuntime(context,
                 SocketType.ROUTER);
             Message attempted = Message.from("attempted");
             Message later = Message.from("not-attempted")) {
            router.setOption(SocketOptions.ROUTER_MANDATORY, 1);

            SendResult result = router.sendNoWaitPartsResult(
                RoutingId.from("missing-route"), List.of(attempted, later));

            assertEquals(SendResult.NOT_READY, result);
            assertEquals(0, attempted.size());
            assertEquals("not-attempted", later.toUtf8String());
        }
    }

    @Test
    void onlyStreamBackpressureRetainsSubmittedPart() {
        assertTrue(SocketSendPlane.submittedPartRetainsCallerOwnership(
            SocketType.STREAM, SubmitResult.BACKPRESSURED.value(),
            NativeErrno.EAGAIN));
        assertFalse(SocketSendPlane.submittedPartRetainsCallerOwnership(
            SocketType.STREAM, SubmitResult.BACKPRESSURED.value(),
            NativeErrno.EINTR));
        assertFalse(SocketSendPlane.submittedPartRetainsCallerOwnership(
            SocketType.ROUTER, SubmitResult.BACKPRESSURED.value(),
            NativeErrno.EAGAIN));
        assertFalse(SocketSendPlane.submittedPartRetainsCallerOwnership(
            SocketType.STREAM, SubmitResult.NOT_CONNECTED.value(),
            NativeErrno.EAGAIN));
    }
}
