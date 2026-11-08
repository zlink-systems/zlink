/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.*;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.CloseResult;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.runtime.nativeapi.NativeErrno;

class SubmitCloseAdmissionContractTest {
    @Test
    void nativeBusyCloseKeepsHandleOpen() throws Exception {
        CompletionNativeFixture.runProbe(getClass());
    }

    public static void main(String[] args) throws Throwable {
        CompletionNativeFixture core = new CompletionNativeFixture();
        try (Context context = Zlink.createContext();
             DealerSocket dealer = context.createDealerSocket()) {
            // No binding call holds a reservation. Core itself rejects close.
            core.admissionEntered.countDown();
            core.closeBusyDuringAdmission = true;
            try {
                ZlinkCloseException failure = assertThrows(
                    ZlinkCloseException.class, dealer::close);
                assertEquals(CloseResult.BUSY, failure.getResult());
                assertEquals(NativeErrno.EBUSY, failure.getNativeErrno());
            } finally {
                core.closeBusyDuringAdmission = false;
            }
            core.attempts.add(new CompletionNativeFixture.Attempt(
                SubmitResult.OK, 0, 0));
            try (Message message = Message.from("after-native-busy")) {
                dealer.send().message(message).submit_sync();
            }
            core.verify(0);
        }
    }

    @Test
    void closedSocketRejectsEverySubmitWithoutConsumingMessages() {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             RouterSocket router = context.createRouterSocket();
             DealerSocket dealer = context.createDealerSocket();
             DealerSocket ownerless = context.createDealerSocket();
             Poller poller = Zlink.createPoller();
             Received received = new Received();
             Message message = Message.from("retained")) {
            String endpoint = TestSupport.inprocEndpoint("closed-submits");
            router.bind(endpoint);
            dealer.connect(endpoint);
            poller.add(dealer, 1, PollEventFlags.POLLCOMPLETION);
            try (Message request = Message.from("request")) {
                dealer.request().message(request)
                    .timeout(Duration.ofSeconds(5)).submit();
            }
            assertTrue(router.recv(received, RecvFlags.NONE));
            var send = dealer.send().message(message);
            var request = dealer.request().message(message);
            var reply = received.reply().message(message);
            var explicitReply = router.reply(
                received.getRoutingId().orElseThrow(),
                received.replyToken().orElseThrow()).message(message);
            router.close();
            dealer.close();
            ownerless.close();
            assertTerminated(send::submit);
            assertTerminated(request::submit);
            assertTerminated(reply::submit);
            assertTerminated(explicitReply::submit);
            assertTerminated(() -> dealer.send().message(message)
                .submit_sync());
            assertTerminated(() -> dealer.request().message(message)
                .submit_sync());
            assertTerminated(() -> ownerless.request().message(message)
                .submit());
            assertEquals("retained", message.toUtf8String());
        }
    }

    @Test
    void failedNativeActionReleasesItsReservation() {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             DealerSocket dealer = context.createDealerSocket()) {
            NativeSocketRuntime runtime = ((NativeSocketBase) dealer).runtime();
            CompletionOwner owner = runtime.completionOwner();
            SocketCore core = runtime.socketCore();
            RuntimeException failure = new RuntimeException(
                "native preparation failed");
            assertSame(failure, assertThrows(RuntimeException.class,
                () -> core.withNativeCall(() -> {
                    throw failure;
                })));
            assertSame(failure, assertThrows(RuntimeException.class,
                () -> owner.trackNoWaitSend((contextToken, idOut) -> {
                    throw failure;
                })));
            dealer.close();
            assertTerminated(() -> core.withNativeCall(() -> {
                fail("released handle must never be used");
                return null;
            }));
            assertTerminated(() ->
                owner.trackNoWaitSend((contextToken, idOut) -> {
                    fail("released handle must never be used");
                    return SubmitResult.OK.value();
                }));
        }
    }

    @Test
    void failedPreparationReleasesTokenAndPendingReservations() {
        TestSupport.assumeNative();
        assertPreparationFailureReleases(owner ->
            owner.submitSend(null, List.of()));
        assertPreparationFailureReleases(owner ->
            owner.submitSendBlocking(null, List.of()));
        assertPreparationFailureReleases(owner ->
            owner.submitRequest(null, List.of(), Duration.ZERO));
        assertPreparationFailureReleases(owner ->
            owner.submitRequestBlocking(null, List.of(), Duration.ZERO));
        // Timeout conversion fails before submitPartsAttempt is entered.
        assertPreparationFailureReleases(owner ->
            owner.submitRequestBlocking(null, List.of(),
                Duration.ofMillis((long) Integer.MAX_VALUE + 1)));
        assertPreparationFailureReleases(owner ->
            owner.submitReply(null, 1L, List.of()));
    }

    private static void assertPreparationFailureReleases(
        Consumer<CompletionOwner> submit) {
        try (Context context = Zlink.createContext();
             DealerSocket dealer = context.createDealerSocket();
             Poller poller = Zlink.createPoller()) {
            poller.add(dealer, 1, PollEventFlags.POLLCOMPLETION);
            CompletionOwner owner = ((NativeSocketBase) dealer).runtime()
                .completionOwner();
            assertThrows(IllegalArgumentException.class,
                () -> submit.accept(owner));
            dealer.close();
        }
    }

    @Test
    void closeCannotReleaseHandleBetweenBindingAdmissionAndCoreEntry()
        throws Exception {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             DealerSocket dealer = context.createDealerSocket()) {
            SocketCore core = ((NativeSocketBase) dealer).runtime()
                .socketCore();
            CountDownLatch entered = new CountDownLatch(1);
            CountDownLatch release = new CountDownLatch(1);
            FutureTask<Void> submit = new FutureTask<>(() ->
                core.withNativeCall(() -> {
                    entered.countDown();
                    try {
                        assertTrue(release.await(TestSupport.DEFAULT_TIMEOUT_MS,
                            TimeUnit.MILLISECONDS));
                    } catch (InterruptedException failure) {
                        Thread.currentThread().interrupt();
                        throw new AssertionError(failure);
                    }
                    return null;
                }));
            Thread sender = Thread.ofPlatform().start(submit);
            try {
                assertTrue(entered.await(TestSupport.DEFAULT_TIMEOUT_MS,
                    TimeUnit.MILLISECONDS));
                ZlinkCloseException busy = assertThrows(
                    ZlinkCloseException.class, dealer::close);
                assertEquals(CloseResult.BUSY, busy.getResult());
                assertEquals(NativeErrno.EBUSY, busy.getNativeErrno());
            } finally {
                release.countDown();
                sender.join(TestSupport.DEFAULT_TIMEOUT_MS);
            }
            submit.get(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
            dealer.close();
            ZlinkSubmitException terminated = assertThrows(
                ZlinkSubmitException.class, () -> core.withNativeCall(() -> {
                    fail("closed socket must not enter the native action");
                    return null;
                }));
            assertEquals(SubmitResult.TERMINATED, terminated.getResult());
        }
    }

    private static void assertTerminated(Runnable submit) {
        ZlinkSubmitException failure = assertThrows(
            ZlinkSubmitException.class, submit::run);
        assertEquals(SubmitResult.TERMINATED, failure.getResult());
        assertEquals(NativeErrno.ESHUTDOWN, failure.getNativeErrno());
    }
}
