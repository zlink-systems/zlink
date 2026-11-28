/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.*;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.runtime.nativeapi.NativeErrno;

class InvalidWaitTokenErrnoContractTest {
    @Test
    void invalidWaitTokenErrnoFailsSendAndRequest() throws Exception {
        CompletionNativeFixture.runProbe(getClass());
    }

    public static void main(String[] args) throws Throwable {
        CompletionNativeFixture core = new CompletionNativeFixture();
        byte[] payload = "caller payload".getBytes(StandardCharsets.UTF_8);
        try (Context context = Zlink.createContext();
             DealerSocket dealer = context.createDealerSocket()) {
            CompletionOwner owner = CompletionNativeFixture.claim((NativeSocketBase) dealer);
            Map<?, ?> pending = (Map<?, ?>) field(owner, "pending");
            long nextId = 41;
            for (boolean request : new boolean[] {false, true}) {
                CompletionNativeFixture.Submission rejectedInitial;
                CompletionNativeFixture.Submission rejectedRetry;
                try (Message message = Message.from(payload)) {
                    scriptBackpressure(core, nextId++, NativeErrno.EINVAL);
                    assertProtocolError(assertThrows(ZlinkSubmitException.class,
                        () -> submit(dealer, message, request)));
                    assertArrayEquals(payload, message.data());
                    assertTrue(pending.isEmpty());
                    assertEquals(0, field(owner, "unpublishedSendCount"));
                    rejectedInitial = core.submissions.getLast();
                }

                try (Message message = Message.from(payload)) {
                    scriptBackpressure(core, nextId++, NativeErrno.EAGAIN);
                    var admitted = submit(dealer, message, request);
                    assertFalse(admitted.isDone());
                    var state = pending.values().iterator().next();
                    Message staged = (Message) ((List<?>) field(state, "retained")).getFirst();
                    assertArrayEquals(payload, staged.data());
                    scriptBackpressure(core, nextId++, NativeErrno.EINVAL);
                    core.writable(core.submissions.getLast(), 0, 0);
                    assertEquals(1, owner.drain());
                    assertProtocolError(assertInstanceOf(ZlinkSubmitException.class,
                        CompletionNativeFixture.failure(admitted)));
                    assertTrue(pending.isEmpty());
                    assertTrue(((List<?>) field(state, "retained")).isEmpty());
                    assertEquals(0, staged.size());
                    rejectedRetry = core.submissions.getLast();
                }
                assertLateRecordsCloseDuringSubmit(core, dealer, owner, request,
                    nextId++, rejectedInitial, rejectedRetry);
                assertTrue(pending.isEmpty());
                assertEquals(0, field(owner, "unpublishedSendCount"));
                assertTrue(((Map<?, ?>) field(owner, "earlyWritable")).isEmpty());
            }
            core.verify(8);
        }
    }

    private static void assertLateRecordsCloseDuringSubmit(
            CompletionNativeFixture core, DealerSocket dealer,
            CompletionOwner owner, boolean request, long id,
            CompletionNativeFixture.Submission rejectedInitial,
            CompletionNativeFixture.Submission rejectedRetry) throws Exception {
        core.armAdmissionGate();
        core.attempts.add(new CompletionNativeFixture.Attempt(
            SubmitResult.BACKPRESSURED, NativeErrno.EAGAIN, id, true));
        try (Message message = Message.from("next")) {
            var submit = new FutureTask<>(() -> submit(dealer, message, request));
            var drain = new FutureTask<>(owner::drain);
            Thread submitter = Thread.ofPlatform().daemon().start(submit);
            Thread drainer = null;
            try {
                assertTrue(core.admissionEntered.await(
                    TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS));
                var later = core.submissions.getLast();
                assertNotEquals(rejectedInitial.context(), later.context());
                assertNotEquals(rejectedRetry.context(), later.context());
                core.writable(rejectedInitial, 0, 0);
                core.requestResult(rejectedRetry, RequestResult.OK);
                long closedBefore = CompletionOwner.closedCompletionCount();
                drainer = Thread.ofPlatform().daemon().start(drain);
                assertEquals(2, drain.get(TestSupport.DEFAULT_TIMEOUT_MS,
                    TimeUnit.MILLISECONDS));
                assertEquals(closedBefore + 2, CompletionOwner.closedCompletionCount());
                assertFalse(submit.isDone());
            } finally {
                core.releaseAdmission.countDown();
                submitter.join(TestSupport.DEFAULT_TIMEOUT_MS);
                if (drainer != null)
                    drainer.join(TestSupport.DEFAULT_TIMEOUT_MS);
            }

            var admitted = submit.get(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
            assertFalse(admitted.isDone());
            core.writable(core.submissions.getLast(), 801, NativeErrno.ENOENT);
            assertEquals(1, owner.drain());
            var failure = assertInstanceOf(ZlinkSubmitException.class,
                CompletionNativeFixture.failure(admitted));
            assertEquals(SubmitResult.NOT_FOUND, failure.getResult());
            assertEquals(NativeErrno.ENOENT, failure.getNativeErrno());
        }
    }

    private static CompletableFuture<Void> submit(DealerSocket dealer,
                                                   Message message,
                                                   boolean request) {
        return (request ? dealer.request().message(message).submit().admitted()
            : dealer.send().message(message).submit().admitted()).toCompletableFuture();
    }

    private static void scriptBackpressure(CompletionNativeFixture core, long id,
                                           int errno) {
        core.attempts.add(new CompletionNativeFixture.Attempt(
            SubmitResult.BACKPRESSURED, errno, id));
    }

    private static void assertProtocolError(ZlinkSubmitException error) {
        assertEquals(SubmitResult.INTERNAL_ERROR, error.getResult());
        assertEquals(NativeErrno.EPROTO, error.getNativeErrno());
    }

    private static Object field(Object target, String name) throws ReflectiveOperationException {
        var field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(target);
    }
}
