/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.*;

import java.util.AbstractList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.CloseResult;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.PubSocket;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.contracts.sockets.XPubSocket;
import systems.zlink.runtime.nativeapi.NativeErrno;

class PublishCloseAdmissionContractTest {
    @Test
    void preparingPublishKeepsHandleAndPermitsAnotherPublish()
        throws Exception {
        assertAdmission(SendFlag.NONE);
    }

    @Test
    void preparingNoWaitPublishKeepsHandleAndPermitsAnotherPublish()
        throws Exception {
        assertAdmission(SendFlag.DONTWAIT);
    }

    private static void assertAdmission(SendFlag flag) throws Exception {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             PubSocket pub = context.createPubSocket();
             Message first = Message.from("first");
             Message second = Message.from("second")) {
            pub.bind(TestSupport.inprocEndpoint("publish-admission"));
            NativeSocketRuntime runtime = ((NativeSocketBase) pub).runtime();
            CountDownLatch entered = new CountDownLatch(1);
            CountDownLatch release = new CountDownLatch(1);
            AtomicInteger reads = new AtomicInteger();
            List<Message> parts = new AbstractList<>() {
                @Override
                public Message get(int index) {
                    // Validation reads once; the move reads inside admission.
                    if (reads.incrementAndGet() == 2) {
                        entered.countDown();
                        try {
                            assertTrue(release.await(
                                TestSupport.DEFAULT_TIMEOUT_MS,
                                TimeUnit.MILLISECONDS));
                        } catch (InterruptedException failure) {
                            Thread.currentThread().interrupt();
                            throw new AssertionError(failure);
                        }
                    }
                    return first;
                }

                @Override
                public int size() {
                    return 1;
                }
            };
            FutureTask<Void> blocked = new FutureTask<>(() -> {
                if (flag == SendFlag.DONTWAIT) {
                    runtime.publishNoWaitPartsResult("race", parts);
                } else {
                    runtime.publishParts("race", parts, flag);
                }
                return null;
            });
            FutureTask<Void> concurrent = new FutureTask<>(() -> {
                pub.publish("race").message(second)
                    .flags(SendFlags.DONT_WAIT).submit();
                return null;
            });
            Thread sender = Thread.ofPlatform().start(blocked);
            Thread other = null;
            try {
                assertTrue(entered.await(TestSupport.DEFAULT_TIMEOUT_MS,
                    TimeUnit.MILLISECONDS));
                other = Thread.ofPlatform().start(concurrent);
                concurrent.get(TestSupport.DEFAULT_TIMEOUT_MS,
                    TimeUnit.MILLISECONDS);
                assertFalse(blocked.isDone());
                ZlinkCloseException busy = assertThrows(
                    ZlinkCloseException.class, pub::close);
                assertEquals(CloseResult.BUSY, busy.getResult());
                assertEquals(NativeErrno.EBUSY, busy.getNativeErrno());
            } finally {
                release.countDown();
                sender.join(TestSupport.DEFAULT_TIMEOUT_MS);
                if (other != null)
                    other.join(TestSupport.DEFAULT_TIMEOUT_MS);
            }
            blocked.get(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
            pub.close();
        }
    }

    @Test
    void closedPublishRejectsEveryEntryWithoutConsumingMessages() {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             PubSocket pub = context.createPubSocket();
             XPubSocket xpub = context.createXPubSocket();
             Message part = Message.from("retained")) {
            NativeSocketRuntime runtime = ((NativeSocketBase) pub).runtime();
            var publish = pub.publish("race").message(part);
            var noWait = pub.publish("race").message(part)
                .flags(SendFlags.DONT_WAIT);
            pub.close();
            xpub.close();
            assertTerminated(publish::submit);
            assertTerminated(noWait::submit);
            assertTerminated(() -> runtime.publishMessageFrame(
                "race", part, SendFlag.NONE));
            assertTerminated(() -> runtime.publishMessageFrameNoWaitResult(
                "race", part));
            assertTerminated(() -> xpub.publish("race").message(part).submit());
            assertTerminated(() -> xpub.publish("race").message(part)
                .flags(SendFlags.DONT_WAIT).submit());
            assertEquals("retained", part.toUtf8String());
        }
    }

    @Test
    void failedPublishPreparationReleasesReservation() {
        TestSupport.assumeNative();
        for (SendFlag flag : List.of(SendFlag.NONE, SendFlag.DONTWAIT)) {
            try (Context context = Zlink.createContext();
                 PubSocket pub = context.createPubSocket();
                 Message part = Message.from("retained")) {
                NativeSocketRuntime runtime =
                    ((NativeSocketBase) pub).runtime();
                RuntimeException failure = new RuntimeException(
                    "publish preparation failed");
                AtomicInteger reads = new AtomicInteger();
                List<Message> parts = new AbstractList<>() {
                    @Override
                    public Message get(int index) {
                        if (reads.incrementAndGet() == 2)
                            throw failure;
                        return part;
                    }

                    @Override
                    public int size() {
                        return 1;
                    }
                };
                assertSame(failure, assertThrows(RuntimeException.class, () -> {
                    if (flag == SendFlag.DONTWAIT) {
                        runtime.publishNoWaitPartsResult("race", parts);
                    } else {
                        runtime.publishParts("race", parts, flag);
                    }
                }));
                pub.close();
                assertEquals("retained", part.toUtf8String());
            }
        }
    }

    private static void assertTerminated(Runnable publish) {
        ZlinkSubmitException failure = assertThrows(
            ZlinkSubmitException.class, publish::run);
        assertEquals(SubmitResult.TERMINATED, failure.getResult());
        assertEquals(NativeErrno.ESHUTDOWN, failure.getNativeErrno());
    }
}
