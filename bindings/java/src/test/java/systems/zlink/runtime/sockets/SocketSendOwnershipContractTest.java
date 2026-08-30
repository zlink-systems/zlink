/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTimeoutPreemptively;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.TimeUnit;
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
    void asyncMultipartValidatesNullBeforeTransferringPrefix() {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             NativeSocketRuntime dealer = new NativeSocketRuntime(context,
                 SocketType.DEALER);
             Message first = Message.from("first")) {
            NullPointerException failure = assertThrows(
                NullPointerException.class,
                () -> dealer.sendAsync(Arrays.asList(first, null), null));

            assertEquals("parts[1]", failure.getMessage());
            assertEquals("first", first.toUtf8String());
            assertFalse(first.more());
        }
    }

    @Test
    void asyncMultipartTransferFailureRestoresMovedPrefixAndAllowsRetry()
        throws Exception {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             NativeSocketRuntime sender = new NativeSocketRuntime(context,
                 SocketType.PAIR);
             NativeSocketRuntime receiver = new NativeSocketRuntime(context,
                 SocketType.PAIR);
             Message first = Message.from("first");
             Message second = Message.from("second")) {
            String endpoint = TestSupport.inprocEndpoint(
                "async-send-transfer-failure-retry");
            receiver.bind(endpoint);
            sender.connect(endpoint);

            second.close();
            assertThrows(
                IllegalStateException.class,
                () -> sender.sendAsync(List.of(first, second), null));

            assertEquals("first", first.toUtf8String());
            assertFalse(first.more());

            try (Message retry = Message.from("retry")) {
                sender.sendAsync(List.of(retry), null).toCompletableFuture()
                    .get(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
                try (systems.zlink.contracts.messaging.Received received =
                         receiver.recv()) {
                    assertEquals("retry",
                        received.singlePartOrThrow().toUtf8String());
                }
            }
        }
    }

    @Test
    void closedMessageCannotAliasReusedSlotDuringAsyncSend() throws Exception {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             NativeSocketRuntime sender = new NativeSocketRuntime(context,
                 SocketType.PAIR);
             NativeSocketRuntime receiver = new NativeSocketRuntime(context,
                 SocketType.PAIR)) {
            String endpoint = TestSupport.inprocEndpoint(
                "async-send-closed-slot-alias");
            receiver.bind(endpoint);
            sender.connect(endpoint);

            Message stale = Message.from("stale");
            stale.close();
            try (Message live = Message.from("live")) {
                IllegalStateException failure = assertThrows(
                    IllegalStateException.class,
                    () -> sender.sendAsync(List.of(stale), null));

                assertEquals("message is closed", failure.getMessage());
                assertThrows(IllegalStateException.class, stale::refCount);
                assertEquals("live", live.toUtf8String(),
                    "a stale wrapper must not move the reused native slot");

                sender.sendAsync(List.of(live), null).toCompletableFuture()
                    .get(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
                try (systems.zlink.contracts.messaging.Received received =
                         receiver.recv()) {
                    assertEquals("live",
                        received.singlePartOrThrow().toUtf8String());
                }
            }
        }
    }

    @Test
    void transferredMessageCannotBeSubmittedAgainOnSyncPath() {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             NativeSocketRuntime sender = new NativeSocketRuntime(context,
                 SocketType.PAIR);
             NativeSocketRuntime receiver = new NativeSocketRuntime(context,
                 SocketType.PAIR);
             Message transferred = Message.from("first")) {
            String endpoint = TestSupport.inprocEndpoint(
                "sync-send-transferred-message");
            receiver.bind(endpoint);
            sender.connect(endpoint);

            sender.send(List.of(transferred), SendFlag.NONE);
            try (systems.zlink.contracts.messaging.Received received =
                     receiver.recv()) {
                assertEquals("first",
                    received.singlePartOrThrow().toUtf8String());
            }

            IllegalStateException failure = assertThrows(
                IllegalStateException.class,
                () -> sender.send(List.of(transferred), SendFlag.NONE));
            assertEquals("message is closed", failure.getMessage());
        }
    }

    @Test
    void coreRejectedAsyncMultipartRestoresAllSourceParts() {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             NativeSocketRuntime dealer = new NativeSocketRuntime(context,
                 SocketType.DEALER);
             Message first = Message.from("first");
             Message second = Message.from("second")) {
            assertThrows(ZlinkSubmitException.class,
                () -> dealer.sendAsync(List.of(first, second), null));

            assertEquals("first", first.toUtf8String());
            assertTrue(first.more());
            assertEquals("second", second.toUtf8String());
            assertFalse(second.more());
        }
    }

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
