/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import java.lang.reflect.Field;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Queue;
import java.util.concurrent.FutureTask;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.jupiter.api.Test;
import systems.zlink.MessageSlotFixture;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.CloseResult;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.internal.ContractAccess;
import systems.zlink.runtime.nativeapi.NativeErrno;

import static org.junit.jupiter.api.Assertions.*;

class MessageCopyRetryContractTest {
    @Test
    void retryRetainsHeaderThroughTerminalRaceUnderLoad() throws Exception {
        MessageSlotFixture.runProbe(getClass());
    }

    public static void main(String[] args) throws Throwable {
        CompletionNativeFixture core = new CompletionNativeFixture();
        MessageSlotFixture counts = new MessageSlotFixture();
        AtomicBoolean running = new AtomicBoolean(true);
        CountDownLatch warmed = new CountDownLatch(2);
        List<FutureTask<Void>> loads = new ArrayList<>();
        List<Thread> workers = new ArrayList<>();
        for (int i = 0; i < 2; i++) {
            FutureTask<Void> load = new FutureTask<>(() -> {
                try (Message source = Message.from(new byte[1024])) {
                    source.copy().close();
                    warmed.countDown();
                    while (running.get()) {
                        try (Message copy = source.copy()) {
                            assertEquals(1024, copy.size());
                        }
                    }
                }
                return null;
            });
            loads.add(load);
            workers.add(Thread.ofPlatform().start(load));
        }
        try {
            assertTrue(warmed.await(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS));
            for (int iteration = 0; iteration < 200; iteration++) race(core, counts, false);
            race(core, counts, true);
        } finally {
            running.set(false);
            for (Thread worker : workers) worker.join(TestSupport.DEFAULT_TIMEOUT_MS);
            for (FutureTask<Void> load : loads) load.get(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
        }
        core.verify(201);
        System.out.println("retry barrier + terminal/socket-close race under two copy loads: 200 PASS; forced early return: detected");
    }

    private static Object field(Object target, String name) throws ReflectiveOperationException {
        Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(target);
    }

    private static long address(Message message) {
        return ContractAccess.nativeMessageAddress(ContractAccess.messageNativeHandle(message));
    }

    private static Message retained(CompletionOwner owner) throws ReflectiveOperationException {
        Map<?, ?> pending = (Map<?, ?>) field(owner, "pending");
        List<?> parts = (List<?>) field(pending.values().iterator().next(), "retained");
        return (Message) parts.getFirst();
    }

    private static void assertUnavailable(long retained, List<Message> acquired) {
        for (int i = 0; i < 64; i++) {
            Message next = new Message(1024);
            acquired.add(next);
            assertNotEquals(retained, address(next), "active retry header was reissued");
        }
    }

    private static void race(CompletionNativeFixture core, MessageSlotFixture counts,
                             boolean forceEarlyReturn) throws Exception {
        core.armAdmissionGate();
        try (Context context = Zlink.createContext();
             DealerSocket dealer = context.createDealerSocket();
             Message source = Message.from(new byte[1024])) {
            CompletionOwner owner = CompletionNativeFixture.claim((NativeSocketBase) dealer);
            core.attempts.add(new CompletionNativeFixture.Attempt(
                SubmitResult.BACKPRESSURED, NativeErrno.EAGAIN, 41));
            var reply = dealer.request().message(source.copy())
                .timeout(Duration.ofSeconds(2)).submit().reply().toCompletableFuture();
            Message part = retained(owner);
            long header = address(part);
            int closes = counts.closes(header);
            assertEquals(2, source.refCount());
            core.writable(core.submissions.getLast(), 0, 0);
            core.attempts.add(new CompletionNativeFixture.Attempt(SubmitResult.OK, 0, 42, true));
            FutureTask<Integer> drain = new FutureTask<>(owner::drain);
            Thread drainer = Thread.ofPlatform().start(drain);
            List<Message> acquired = new ArrayList<>();
            int closesAfterRetry;
            try {
                assertTrue(core.admissionEntered.await(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS));
                assertEquals(CloseResult.BUSY,
                    assertThrows(ZlinkCloseException.class, dealer::close).getResult());
                // Exercise the terminal notification while Pending.retryActive is true.
                owner.rejectPendingClosed();
                assertFalse(reply.isDone());
                assertEquals(closes, counts.closes(header));
                assertEquals(2, source.refCount());
                if (forceEarlyReturn) {
                    part.close();
                    assertThrows(AssertionError.class, () -> assertUnavailable(header, acquired));
                } else {
                    assertUnavailable(header, acquired);
                    assertArrayEquals(new byte[1024], part.data());
                }
                assertFalse(drain.isDone());
            } finally {
                core.releaseAdmission.countDown();
                drainer.join(TestSupport.DEFAULT_TIMEOUT_MS);
                closesAfterRetry = counts.closes(header);
                Message.closeAll(acquired);
            }
            assertEquals(1, drain.get(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS));
            assertEquals(RequestResult.TERMINATED, assertInstanceOf(ZlinkRequestException.class,
                CompletionNativeFixture.failure(reply)).getResult());
            assertEquals(closes + 1, closesAfterRetry, "retained payload must close once");
            assertEquals(1, source.refCount());
            if (!forceEarlyReturn) {
                Field sharedField = Class.forName("systems.zlink.contracts.messaging.Message$MessageSlotPool")
                    .getDeclaredField("SHARED");
                sharedField.setAccessible(true);
                Queue<?> shared = (Queue<?>) sharedField.get(null);
                // The warmed load threads reuse their own local slots.
                assertEquals(1, shared.stream().filter(slot -> {
                    try {
                        return ContractAccess.nativeMessageAddress(field(slot, "handle")) == header;
                    } catch (ReflectiveOperationException failure) {
                        throw new AssertionError(failure);
                    }
                }).count(), "retained header must be returned exactly once");
            }
            dealer.close();
        }
    }
}
