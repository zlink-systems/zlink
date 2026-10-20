/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import com.sun.net.httpserver.HttpServer;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext;

import java.net.InetSocketAddress;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

final class HttpYieldAdmissionTest {
    @Test
    void closedCallbackQueueReportsAdmissionFailure() throws Exception {
        callbackFailureIsReported(true);
    }

    @Test
    void throwingCallbackReportsOriginalFailure() throws Exception {
        callbackFailureIsReported(false);
    }

    private static void callbackFailureIsReported(boolean closeBeforeCompletion) throws Exception {
        var release = new CompletableFuture<Void>();
        var executor = new ManualExecutor();
        var queue = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.spot());
        var records = new java.util.concurrent.CopyOnWriteArrayList<java.util.logging.LogRecord>();
        var failureReported = new CompletableFuture<Void>();
        var logger = java.util.logging.Logger.getLogger("systems.zlink.framework");
        var handler =
                new java.util.logging.Handler() {
                    public void publish(java.util.logging.LogRecord record) {
                        records.add(record);
                        failureReported.complete(null);
                    }

                    public void flush() {}

                    public void close() {}
                };
        var failure = new IllegalStateException("callback failed");
        var callbacks = new AtomicInteger();
        var callbackInvoked = new CompletableFuture<Void>();
        var frameworkTurn = new ZLinkFrameworkHttpExecutionTurn();
        logger.addHandler(handler);
        try (var server = new HeldServer(release);
                var client = ZLinkHttpClient.create(server.baseUrl()).buildServer(frameworkTurn)) {
            var owner =
                    queue.enqueue(
                            () -> {
                                try (var scope =
                                        ZLinkSuspendInvocationContext.enterApplicationExecution(
                                                application())) {
                                    client.get("/callback")
                                            .submit(
                                                    String.class,
                                                    (error, response) -> {
                                                        callbacks.incrementAndGet();
                                                        callbackInvoked.complete(null);
                                                        throw failure;
                                                    });
                                    return CompletableFuture.completedFuture(null);
                                }
                            },
                            null);
            executor.runUntil(owner.toCompletableFuture());
            owner.toCompletableFuture().join();
            if (closeBeforeCompletion) queue.sealClosingAdmission();
            release.complete(null);
            if (!closeBeforeCompletion) {
                executor.runUntil(callbackInvoked);
            }
            failureReported.get(5, java.util.concurrent.TimeUnit.SECONDS);
            client.close();
            assertEquals(closeBeforeCompletion ? 0 : 1, callbacks.get());
            assertEquals(1, records.size(), "callback failure must reach runtime error reporting");
            var reported = records.getFirst().getThrown();
            assertNotNull(reported);
            if (closeBeforeCompletion) {
                assertEquals(
                        ZLinkFrameworkErrorKind.REJECTED,
                        ((ZLinkFrameworkException) reported).kind());
            } else {
                assertSame(failure, reported);
            }
        } finally {
            release.complete(null);
            executor.drain();
            queue.close();
            logger.removeHandler(handler);
        }
    }

    @Test
    void factoryFailureResumesThroughYieldBeforeAnotherYield() throws Exception {
        var executor = new ManualExecutor();
        var queue = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.spot());
        var release = new CompletableFuture<Void>();
        var blockerStarted = new CompletableFuture<Void>();
        var failure = new IllegalStateException("request factory failed");
        var synchronousFailure = new AtomicReference<Throwable>();
        var calls = new AtomicInteger();
        var events = new ArrayList<String>();
        var turn = new ZLinkFrameworkHttpExecutionTurn();
        var owner =
                queue.enqueue(
                        () -> {
                            try (var scope =
                                    ZLinkSuspendInvocationContext.enterApplicationExecution(
                                            application())) {
                                CompletionStage<Integer> pending;
                                try {
                                    pending =
                                            turn.yield(
                                                    () -> {
                                                        calls.incrementAndGet();
                                                        throw failure;
                                                    });
                                } catch (Throwable error) {
                                    synchronousFailure.set(error);
                                    pending = CompletableFuture.failedFuture(error);
                                }
                                var rejection =
                                        assertThrows(
                                                ZLinkFrameworkException.class,
                                                () ->
                                                        turn.yield(
                                                                () -> {
                                                                    calls.incrementAndGet();
                                                                    return CompletableFuture
                                                                            .completedFuture(0);
                                                                }));
                                assertEquals(
                                        ZLinkFrameworkErrorKind.INVALID_OPERATION,
                                        rejection.kind());
                                return pending.handle(
                                                (value, error) -> {
                                                    assertSame(failure, error);
                                                    events.add("catch");
                                                    return turn.yield(
                                                            () ->
                                                                    CompletableFuture
                                                                            .completedFuture(7));
                                                })
                                        .thenCompose(resumed -> resumed)
                                        .thenAccept(
                                                value -> {
                                                    assertEquals(7, value);
                                                    events.add("end");
                                                });
                            }
                        },
                        null);
        var blocker =
                queue.enqueue(
                        () -> {
                            events.add("blocker");
                            blockerStarted.complete(null);
                            return release.thenRun(() -> events.add("blocker:end"));
                        },
                        null);
        try {
            executor.runUntil(blockerStarted);
            assertNull(synchronousFailure.get(), "factory failure must use the returned stage");
            assertEquals(1, calls.get(), "rejected factory must not run");
            assertEquals(List.of("blocker"), events, "failure continuation must wait for the gate");
            assertFalse(owner.toCompletableFuture().isDone());
            release.complete(null);
            executor.runUntil(owner.toCompletableFuture());
            executor.runUntil(blocker.toCompletableFuture());
            owner.toCompletableFuture().join();
            blocker.toCompletableFuture().join();
            assertEquals(List.of("blocker", "blocker:end", "catch", "end"), events);
        } finally {
            release.complete(null);
            executor.runUntil(owner.toCompletableFuture());
            executor.runUntil(blocker.toCompletableFuture());
            owner.handle((value, error) -> null).toCompletableFuture().join();
            blocker.handle((value, error) -> null).toCompletableFuture().join();
            queue.close();
        }
    }

    @Test
    void secondYieldRejectsBeforeActualHttpSubmission() throws Exception {
        var release = new CompletableFuture<Void>();
        var callsFinished = new CompletableFuture<Void>();
        var rejected = new AtomicReference<ZLinkFrameworkException>();
        try (var server = new HeldServer(release);
                var client =
                        ZLinkHttpClient.create(server.baseUrl())
                                .buildServer(new ZLinkFrameworkHttpExecutionTurn())) {
            var queue = new ZLinkSerialExecutionQueue(ZLinkExecutionLanePolicy.spot());
            try {
                var turn =
                        queue.enqueue(
                                () -> {
                                    try (var scope =
                                            ZLinkSuspendInvocationContext.enterApplicationExecution(
                                                    application())) {
                                        var first = client.get("/first").yield(String.class);
                                        try {
                                            client.get("/second").yield(String.class);
                                        } catch (ZLinkFrameworkException error) {
                                            rejected.set(error);
                                        }
                                        callsFinished.complete(null);
                                        return first.thenAccept(
                                                response -> assertEquals("ok", response.body()));
                                    }
                                },
                                null);
                turn.whenComplete(
                        (value, error) -> {
                            if (error != null) callsFinished.completeExceptionally(error);
                        });
                try {
                    callsFinished.join();
                } finally {
                    release.complete(null);
                }
                turn.toCompletableFuture().join();
            } finally {
                release.complete(null);
                queue.close();
            }
            // Closing drains the actual HTTP operations, including an erroneously submitted second
            // request.
            client.close();
            assertEquals(
                    1,
                    server.requests.get(),
                    "actual HTTP submissions before second-call rejection");
            assertNotNull(rejected.get());
            assertEquals(ZLinkFrameworkErrorKind.INVALID_OPERATION, rejected.get().kind());
        } finally {
            release.complete(null);
        }
    }

    @Test
    void resumedTurnAllowsAnotherHttpYield() throws Exception {
        var release = CompletableFuture.<Void>completedFuture(null);
        try (var server = new HeldServer(release);
                var client =
                        ZLinkHttpClient.create(server.baseUrl())
                                .buildServer(new ZLinkFrameworkHttpExecutionTurn())) {
            var queue = new ZLinkSerialExecutionQueue(ZLinkExecutionLanePolicy.spot());
            try {
                queue.enqueue(
                                () -> {
                                    try (var scope =
                                            ZLinkSuspendInvocationContext.enterApplicationExecution(
                                                    application())) {
                                        return client.get("/first")
                                                .yield(String.class)
                                                .thenCompose(
                                                        response ->
                                                                client.get("/next-turn")
                                                                        .yield(String.class))
                                                .thenAccept(
                                                        response ->
                                                                assertEquals(
                                                                        "ok", response.body()));
                                    }
                                },
                                null)
                        .toCompletableFuture()
                        .join();
                assertEquals(2, server.requests.get());
            } finally {
                queue.close();
            }
        }
    }

    @Test
    void callbackDoesNotReleaseOriginTurn() throws Exception {
        var release = new CompletableFuture<Void>();
        var ownerRelease = new CompletableFuture<Void>();
        var executor = new ManualExecutor();
        var queue = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.spot());
        try (var server = new HeldServer(release);
                var client =
                        ZLinkHttpClient.create(server.baseUrl())
                                .buildServer(new ZLinkFrameworkHttpExecutionTurn())) {
            var callback = new CompletableFuture<Void>();
            var originCarrier = new AtomicReference<Object>();
            var callbackCarrier = new AtomicReference<Object>();
            var callbackApplication = new AtomicReference<Object>();
            var callbackActor = new AtomicReference<String>();
            var callbackFlow = new AtomicReference<ZLinkFlowContext.State>();
            var flow = ZLinkFlowContext.create(ZLinkFlowOrigin.INBOUND);
            var owner =
                    queue.enqueue(
                            () -> {
                                try (var scope =
                                        ZLinkSuspendInvocationContext.enterApplicationExecution(
                                                application())) {
                                    originCarrier.set(
                                            ZLinkSuspendInvocationContext
                                                    .currentSerialExecutionTurn());
                                    try (var actor =
                                                    ZLinkSuspendInvocationContext
                                                            .enterActorDispatch("actor");
                                            var flowScope = ZLinkFlowContext.enter(flow)) {
                                        client.get("/callback")
                                                .submit(
                                                        String.class,
                                                        (error, response) -> {
                                                            callbackCarrier.set(
                                                                    ZLinkSuspendInvocationContext
                                                                            .currentSerialExecutionTurn());
                                                            callbackApplication.set(
                                                                    ZLinkSuspendInvocationContext
                                                                            .currentApplicationExecution());
                                                            callbackActor.set(
                                                                    ZLinkSuspendInvocationContext
                                                                            .currentActorDispatch());
                                                            callbackFlow.set(
                                                                    ZLinkFlowContext.current());
                                                            if (error == null)
                                                                callback.complete(null);
                                                            else
                                                                callback.completeExceptionally(
                                                                        error);
                                                        });
                                    }
                                    return ownerRelease;
                                }
                            },
                            null);
            var sibling = queue.enqueue(() -> CompletableFuture.completedFuture(null), null);
            try {
                executor.drain();
                assertFalse(
                        sibling.toCompletableFuture().isDone(),
                        "callback submission must retain the origin turn");
            } finally {
                release.complete(null);
                ownerRelease.complete(null);
                executor.runUntil(callback);
                callback.join();
                client.close();
                executor.drain();
                queue.close();
            }
            owner.toCompletableFuture().join();
            assertNotNull(callbackApplication.get());
            assertNotNull(callbackCarrier.get());
            assertNotSame(originCarrier.get(), callbackCarrier.get());
            assertEquals("actor", callbackActor.get());
            assertSame(flow, callbackFlow.get());
            assertNull(ZLinkSuspendInvocationContext.currentApplicationExecution());
            assertNull(ZLinkSuspendInvocationContext.currentActorDispatch());
            assertNull(ZLinkFlowContext.current());
        } finally {
            release.complete(null);
            ownerRelease.complete(null);
            executor.drain();
            queue.close();
        }
    }

    @Test
    void ownerlessYieldRejectsWithoutInvokingFactory() {
        var submissions = new AtomicInteger();
        var turn = new ZLinkFrameworkHttpExecutionTurn();
        var error =
                assertThrows(
                        ZLinkFrameworkException.class,
                        () ->
                                turn.yield(
                                        () -> {
                                            submissions.incrementAndGet();
                                            return CompletableFuture.completedFuture(1);
                                        }));
        assertEquals(ZLinkFrameworkErrorKind.INVALID_OPERATION, error.kind());
        assertEquals(0, submissions.get());
    }

    @Test
    void callbackTurnHasItsOwnYieldAdmission() throws Exception {
        var executor = new ManualExecutor();
        var queue = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.spot());
        var origin = new CompletableFuture<Void>();
        var callback = new CompletableFuture<Integer>();
        var submissions = new AtomicInteger();
        var turn = new ZLinkFrameworkHttpExecutionTurn();
        try {
            var owner =
                    queue.enqueue(
                            () -> {
                                try (var scope =
                                        ZLinkSuspendInvocationContext.enterApplicationExecution(
                                                application())) {
                                    turn.callbackScheduler()
                                            .execute(
                                                    () ->
                                                            turn.yield(
                                                                            () -> {
                                                                                submissions
                                                                                        .incrementAndGet();
                                                                                return CompletableFuture
                                                                                        .completedFuture(
                                                                                                7);
                                                                            })
                                                                    .whenComplete(
                                                                            (value, error) -> {
                                                                                if (error == null)
                                                                                    callback
                                                                                            .complete(
                                                                                                    value);
                                                                                else
                                                                                    callback
                                                                                            .completeExceptionally(
                                                                                                    error);
                                                                            }));
                                    return origin;
                                }
                            },
                            null);
            executor.drain();
            assertEquals(0, submissions.get());
            origin.complete(null);
            executor.runUntil(callback);
            assertEquals(7, callback.join());
            executor.drain();
            owner.toCompletableFuture().join();
            assertEquals(1, submissions.get());
        } finally {
            origin.complete(null);
            executor.drain();
            queue.close();
        }
    }

    private static ZLinkSuspendInvocationContext.ApplicationExecution application() {
        return new ZLinkSuspendInvocationContext.ApplicationExecution(
                "room", null, true, true, null);
    }

    private static final class ManualExecutor implements java.util.concurrent.Executor {
        private final java.util.concurrent.LinkedBlockingQueue<Runnable> tasks =
                new java.util.concurrent.LinkedBlockingQueue<>();

        public void execute(Runnable command) {
            tasks.add(command);
        }

        void drain() {
            Runnable command;
            while ((command = tasks.poll()) != null) command.run();
        }

        void runUntil(CompletableFuture<?> completion) throws InterruptedException {
            while (!completion.isDone()) tasks.take().run();
        }
    }

    private static final class HeldServer implements AutoCloseable {
        private final HttpServer server =
                HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
        private final java.util.concurrent.ExecutorService executor =
                Executors.newVirtualThreadPerTaskExecutor();
        final AtomicInteger requests = new AtomicInteger();

        HeldServer(CompletionStage<Void> release) throws Exception {
            server.setExecutor(executor);
            server.createContext(
                    "/",
                    exchange -> {
                        requests.incrementAndGet();
                        release.toCompletableFuture().join();
                        TestSupport.respond(exchange, 200, "\"ok\"");
                    });
            server.start();
        }

        String baseUrl() {
            return "http://127.0.0.1:" + server.getAddress().getPort();
        }

        public void close() {
            server.stop(0);
            executor.close();
        }
    }
}
