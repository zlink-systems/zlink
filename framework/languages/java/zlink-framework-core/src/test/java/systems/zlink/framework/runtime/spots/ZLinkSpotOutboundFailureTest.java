package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext;
import systems.zlink.framework.runtime.messaging.ZLinkStringMessageSerializer;

import java.util.ArrayDeque;
import java.util.List;
import java.util.Map;
import java.util.concurrent.AbstractExecutorService;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkSpotOutboundFailureTest {
    @Test
    void asynchronousOutboundCallbackRetainsTheSerialQueueUntilCompletion() {
        withRuntime(
                (runtime, outbound, executor) -> {
                    var release = new CompletableFuture<Void>();
                    var callbackEntered = new AtomicBoolean();
                    var followingRan = new AtomicBoolean();
                    var queue =
                            new ZLinkSerialExecutionQueue(
                                    executor, ZLinkExecutionLanePolicy.spot());
                    try {
                        var callback =
                                queue.enqueue(
                                        () ->
                                                runtime.runWithOutbound(
                                                        outbound,
                                                        () -> {
                                                            assertTrue(queue.isCurrent());
                                                            assertSame(
                                                                    outbound,
                                                                    ZLinkSuspendInvocationContext
                                                                            .currentSpotOutbound());
                                                            callbackEntered.set(true);
                                                            return release;
                                                        }),
                                        null);
                        var following =
                                queue.enqueue(
                                        () -> {
                                            followingRan.set(true);
                                            return CompletableFuture.completedFuture(null);
                                        },
                                        null);
                        executor.drain();
                        assertTrue(callbackEntered.get());
                        assertFalse(callback.toCompletableFuture().isDone());
                        assertFalse(followingRan.get());
                        assertFalse(following.toCompletableFuture().isDone());
                        assertNull(ZLinkSuspendInvocationContext.currentSpotOutbound());

                        release.complete(null);
                        executor.drain();
                        callback.toCompletableFuture().join();
                        following.toCompletableFuture().join();
                        assertTrue(followingRan.get());
                        assertTrue(queue.awaitQuiescence().toCompletableFuture().isDone());
                    } finally {
                        queue.close();
                    }
                });
    }

    @Test
    void synchronousFailureReturnsFailedStageAndRestoresOutboundScope() {
        withRuntime(
                (runtime, outbound, executor) -> {
                    var failure = new IllegalStateException("synchronous Spot callback failure");
                    CompletionStage<Void> result =
                            assertDoesNotThrow(
                                    () ->
                                            runtime.runWithOutbound(
                                                    outbound,
                                                    () -> {
                                                        assertSame(
                                                                outbound,
                                                                ZLinkSuspendInvocationContext
                                                                        .currentSpotOutbound());
                                                        throw failure;
                                                    }));
                    executor.drain();
                    assertFailure(result, failure);
                    assertNull(ZLinkSuspendInvocationContext.currentSpotOutbound());
                    var following =
                            runtime.runWithOutbound(
                                    outbound,
                                    () -> CompletableFuture.completedFuture("following callback"));
                    executor.drain();
                    assertTrue(following.toCompletableFuture().isDone());
                    assertEquals("following callback", following.toCompletableFuture().join());
                    assertNull(ZLinkSuspendInvocationContext.currentSpotOutbound());
                });
    }

    @Test
    void synchronousFailureReleasesTheSameSerialQueueForTheFollowingCallback() {
        withRuntime(
                (runtime, outbound, executor) -> {
                    var failure = new IllegalStateException("synchronous Spot turn failure");
                    var callbackResult = new AtomicReference<CompletionStage<Void>>();
                    var queue =
                            new ZLinkSerialExecutionQueue(
                                    executor, ZLinkExecutionLanePolicy.spot());
                    try {
                        CompletionStage<Void> failedTurn =
                                queue.enqueue(
                                        () -> {
                                            CompletionStage<Void> result =
                                                    assertDoesNotThrow(
                                                            () ->
                                                                    runtime.runWithOutbound(
                                                                            outbound,
                                                                            () -> {
                                                                                assertTrue(
                                                                                        queue
                                                                                                .isCurrent());
                                                                                assertSame(
                                                                                        outbound,
                                                                                        ZLinkSuspendInvocationContext
                                                                                                .currentSpotOutbound());
                                                                                throw failure;
                                                                            }));
                                            callbackResult.set(result);
                                            return result;
                                        },
                                        null);
                        var followingRan = new AtomicBoolean();
                        var following =
                                queue.enqueue(
                                        () ->
                                                runtime.runWithOutbound(
                                                        outbound,
                                                        () -> {
                                                            assertTrue(queue.isCurrent());
                                                            assertSame(
                                                                    outbound,
                                                                    ZLinkSuspendInvocationContext
                                                                            .currentSpotOutbound());
                                                            followingRan.set(true);
                                                            return CompletableFuture
                                                                    .completedFuture(null);
                                                        }),
                                        null);
                        executor.drain();
                        assertFailure(failedTurn, failure);
                        assertFailure(callbackResult.get(), failure);
                        assertTrue(
                                following.toCompletableFuture().isDone(),
                                "failed callback must release its serial gate");
                        following.toCompletableFuture().join();
                        assertTrue(followingRan.get());
                        assertTrue(queue.awaitQuiescence().toCompletableFuture().isDone());
                        assertFalse(queue.isCurrent());
                        assertNull(ZLinkSuspendInvocationContext.currentSpotOutbound());
                    } finally {
                        queue.close();
                    }
                });
    }

    private static void assertFailure(CompletionStage<?> result, RuntimeException failure) {
        assertTrue(
                result.toCompletableFuture().isDone(),
                "synchronous failure must settle its returned stage");
        CompletionException reported =
                assertThrows(CompletionException.class, () -> result.toCompletableFuture().join());
        assertSame(failure, reported.getCause());
    }

    private static void withRuntime(Scenario scenario) {
        var executor = new ManualExecutor();
        var options = new DefaultZLinkFrameworkOptions();
        options.useHandlerExecutor(executor);
        options.addRouteMesh("mesh");
        var backend = new ZLinkJavaBackendAdapterFactory();
        var adapterOptions =
                new ZLinkBackendAdapterOptions(options.registration().defaultRequestTimeout());
        var serializer = new ZLinkStringMessageSerializer();
        try (var context = backend.createChannelAdapter(adapterOptions).createContext();
                var mesh =
                        backend.createMeshAdapter(adapterOptions).createMeshNode(context, "mesh")) {
            var runtime =
                    new ZLinkSpotRuntime(
                            backend,
                            adapterOptions,
                            options.registration(),
                            null,
                            context,
                            serializer,
                            ZLinkHandlerActivator.reflection(),
                            null,
                            Map.of("mesh", mesh));
            var outbound =
                    new DefaultSpotOutbound(
                            null,
                            "mesh",
                            "channel",
                            serializer,
                            ignored -> null,
                            null,
                            null,
                            null,
                            null,
                            false,
                            options.registration().defaultRequestTimeout(),
                            () -> null,
                            null);
            try {
                scenario.run(runtime, outbound, executor);
            } finally {
                runtime.closeAsync().toCompletableFuture().join();
            }
        }
    }

    @FunctionalInterface
    private interface Scenario {
        void run(ZLinkSpotRuntime runtime, DefaultSpotOutbound outbound, ManualExecutor executor);
    }

    // ExecutorService lets the serial queue submit directly; drain controls when each turn runs.
    private static final class ManualExecutor extends AbstractExecutorService {
        private final ArrayDeque<Runnable> tasks = new ArrayDeque<>();

        @Override
        public void execute(Runnable task) {
            tasks.addLast(task);
        }

        @Override
        public void shutdown() {
            throw new UnsupportedOperationException();
        }

        @Override
        public List<Runnable> shutdownNow() {
            throw new UnsupportedOperationException();
        }

        @Override
        public boolean isShutdown() {
            return false;
        }

        @Override
        public boolean isTerminated() {
            return false;
        }

        @Override
        public boolean awaitTermination(long timeout, TimeUnit unit) {
            throw new UnsupportedOperationException();
        }

        void drain() {
            while (!tasks.isEmpty()) tasks.removeFirst().run();
        }
    }
}
