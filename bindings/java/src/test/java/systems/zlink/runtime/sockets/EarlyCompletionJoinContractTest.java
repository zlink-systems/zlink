/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.*;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.*;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.RequestSubmission;
import systems.zlink.contracts.messaging.SendSubmission;
import systems.zlink.contracts.sockets.*;
import systems.zlink.runtime.nativeapi.NativeErrno;

/**
 * A record read before the native submit returns joins its result without
 * holding a lock across the native call.
 */
class EarlyCompletionJoinContractTest {
    @Test
    void earlyRecordJoinsSubmitAndConcurrentSubmitsOverlap() throws Exception {
        CompletionNativeFixture.runProbe(getClass());
    }

    public static void main(String[] args) throws Throwable {
        CompletionNativeFixture core = new CompletionNativeFixture();
        try (Context context = Zlink.createContext();
             DealerSocket dealer = context.createDealerSocket()) {
            CompletionOwner owner = CompletionNativeFixture.claim(
                (NativeSocketBase) dealer);
            earlyWritableSend(core, dealer, owner);
            earlyRequestReply(core, dealer, owner);
            concurrentSubmits(core, dealer, owner);
            // One WRITABLE record and one REQUEST record were closed.
            core.verify(2);
        }
    }

    private static Thread start(FutureTask<?> task) {
        return Thread.ofPlatform().start(task);
    }

    /** The drain waits for an early record's submit result. */
    private static void awaitJoinWait(Thread drainer) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(
            TestSupport.DEFAULT_TIMEOUT_MS);
        while (drainer.getState() != Thread.State.WAITING) {
            assertTrue(System.nanoTime() < deadline,
                "the drain must wait for the submit result: "
                    + drainer.getState());
            Thread.sleep(5);
        }
    }

    private static void earlyWritableSend(CompletionNativeFixture core,
            DealerSocket dealer, CompletionOwner owner) throws Throwable {
        core.attempts.add(new CompletionNativeFixture.Attempt(
            SubmitResult.BACKPRESSURED, NativeErrno.EAGAIN, 41, true));
        FutureTask<SendSubmission> submit = new FutureTask<>(() ->
            dealer.send().message(Message.from("early")).submit());
        Thread submitter = start(submit);
        assertTrue(core.admissionEntered.await(TestSupport.DEFAULT_TIMEOUT_MS,
            TimeUnit.MILLISECONDS));
        // The WRITABLE record exists before submit returns.
        core.writable(core.submissions.getLast(), 0, 0);
        core.attempts.add(new CompletionNativeFixture.Attempt(
            SubmitResult.OK, 0, 0));
        FutureTask<Integer> drain = new FutureTask<>(owner::drain);
        Thread drainer = start(drain);
        try {
            awaitJoinWait(drainer);
            assertFalse(drain.isDone(),
                "the owner retains the early WRITABLE until publication");
            assertFalse(submit.isDone());
        } finally {
            core.releaseAdmission.countDown();
        }
        SendSubmission submission = submit.get(TestSupport.DEFAULT_TIMEOUT_MS,
            TimeUnit.MILLISECONDS);
        assertEquals(1, drain.get(TestSupport.DEFAULT_TIMEOUT_MS,
            TimeUnit.MILLISECONDS),
            "the owner processes the joined WRITABLE");
        submitter.join(TestSupport.DEFAULT_TIMEOUT_MS);
        drainer.join(TestSupport.DEFAULT_TIMEOUT_MS);
        assertEquals(SubmitResult.BACKPRESSURED, submission.result());
        AtomicInteger terminals = new AtomicInteger();
        var admitted = submission.admitted().toCompletableFuture();
        admitted.whenComplete((ignored, failure) ->
            terminals.incrementAndGet());
        admitted.get(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
        assertEquals(1, terminals.get(), "exactly one terminal");
        assertEquals(2, core.submissions.size(),
            "the joined WRITABLE resubmits the staged message once");
        assertEquals(0, owner.drain(), "no record is left after the join");
    }

    private static void earlyRequestReply(CompletionNativeFixture core,
            DealerSocket dealer, CompletionOwner owner) throws Throwable {
        int before = core.submissions.size();
        core.armAdmissionGate();
        core.attempts.add(new CompletionNativeFixture.Attempt(
            SubmitResult.OK, 0, 51, true));
        FutureTask<RequestSubmission> submit = new FutureTask<>(() ->
            dealer.request().message(Message.from("early"))
                .timeout(Duration.ofSeconds(2)).submit());
        Thread submitter = start(submit);
        assertTrue(core.admissionEntered.await(TestSupport.DEFAULT_TIMEOUT_MS,
            TimeUnit.MILLISECONDS));
        core.requestResult(core.submissions.getLast(), RequestResult.OK);
        FutureTask<Integer> drain = new FutureTask<>(owner::drain);
        Thread drainer = start(drain);
        try {
            awaitJoinWait(drainer);
            assertFalse(drain.isDone());
        } finally {
            core.releaseAdmission.countDown();
        }
        RequestSubmission submission = submit.get(
            TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
        assertEquals(1, drain.get(TestSupport.DEFAULT_TIMEOUT_MS,
            TimeUnit.MILLISECONDS));
        submitter.join(TestSupport.DEFAULT_TIMEOUT_MS);
        drainer.join(TestSupport.DEFAULT_TIMEOUT_MS);
        assertEquals(SubmitResult.OK, submission.result());
        AtomicInteger terminals = new AtomicInteger();
        var reply = submission.reply().toCompletableFuture();
        reply.whenComplete((ignored, failure) -> terminals.incrementAndGet());
        List<Message> parts = reply.get(TestSupport.DEFAULT_TIMEOUT_MS,
            TimeUnit.MILLISECONDS);
        Message.closeAll(parts);
        assertEquals(1, terminals.get(), "exactly one terminal");
        assertEquals(before + 1, core.submissions.size());
    }

    private static void concurrentSubmits(CompletionNativeFixture core,
            DealerSocket dealer, CompletionOwner owner) throws Throwable {
        core.armAdmissionGate();
        core.attempts.add(new CompletionNativeFixture.Attempt(
            SubmitResult.OK, 0, 0, true));
        FutureTask<SendSubmission> first = new FutureTask<>(() ->
            dealer.send().message(Message.from("first")).submit());
        Thread firstThread = start(first);
        assertTrue(core.admissionEntered.await(TestSupport.DEFAULT_TIMEOUT_MS,
            TimeUnit.MILLISECONDS));
        // The first submit is inside the native call. A second submit on the
        // same socket, and a drain, complete without waiting for it.
        core.attempts.add(new CompletionNativeFixture.Attempt(
            SubmitResult.OK, 0, 0));
        FutureTask<SendSubmission> second = new FutureTask<>(() ->
            dealer.send().message(Message.from("second")).submit());
        Thread secondThread = start(second);
        FutureTask<Integer> drain = new FutureTask<>(owner::drain);
        Thread drainer = start(drain);
        try {
            assertEquals(SubmitResult.OK, second.get(
                TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS)
                .result());
            assertEquals(0, drain.get(TestSupport.DEFAULT_TIMEOUT_MS,
                TimeUnit.MILLISECONDS));
            assertFalse(first.isDone(),
                "the first submit is still inside the native call");
        } finally {
            core.releaseAdmission.countDown();
        }
        assertEquals(SubmitResult.OK, first.get(
            TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS).result());
        firstThread.join(TestSupport.DEFAULT_TIMEOUT_MS);
        secondThread.join(TestSupport.DEFAULT_TIMEOUT_MS);
        drainer.join(TestSupport.DEFAULT_TIMEOUT_MS);
    }
}
