package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.*;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.messaging.ZLinkStringMessageSerializer;

import java.util.ArrayDeque;
import java.util.List;
import java.util.OptionalLong;
import java.util.concurrent.AbstractExecutorService;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkChannelHandlerPermitBoundaryTest {
    enum Dispatch {
        HANDLER,
        THROW,
        AWAIT,
        YIELD,
        FILTER_CONTINUE,
        FILTER_STOP,
        FILTER_THROW,
        DECODE_THROW,
        ACTIVATION_THROW
    }

    @ParameterizedTest
    @EnumSource(Dispatch.class)
    void retainsPermitThroughDecodeAndActivationUntilFirstCallback(Dispatch dispatch) {
        var jobs = queue();
        var continuation = new CompletableFuture<Void>();
        var handlerCalls = new AtomicInteger();
        var preparationCalls = new AtomicInteger();
        var handler = new ProbeHandler(jobs, dispatch, continuation, handlerCalls);
        ZLinkHandlerFilter filter =
                new ZLinkHandlerFilter() {
                    @Override
                    public <T> CompletionStage<T> invoke(
                            ZLinkHandlerFilterContext context, ZLinkHandlerFilterNext<T> next) {
                        assertEquals(0, jobs.snapshot().permitsInUse());
                        if (dispatch == Dispatch.FILTER_THROW)
                            throw new IllegalStateException("filter");
                        if (dispatch == Dispatch.FILTER_STOP)
                            return CompletableFuture.completedFuture(null);
                        return next.invoke();
                    }
                };
        boolean filtered =
                switch (dispatch) {
                    case FILTER_CONTINUE, FILTER_STOP, FILTER_THROW -> true;
                    default -> false;
                };
        var serializer =
                new ZLinkMessageSerializer() {
                    @Override
                    public <T> ZLinkEncodedPayload serialize(T value) {
                        return new ZLinkStringMessageSerializer().serialize(value);
                    }

                    @Override
                    public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
                        assertEquals(
                                1,
                                jobs.snapshot().queuedApplicationJobs(),
                                "decode precedes callback");
                        preparationCalls.incrementAndGet();
                        if (dispatch == Dispatch.DECODE_THROW)
                            throw new IllegalStateException("decode");
                        return new ZLinkStringMessageSerializer().deserialize(payload, type);
                    }
                };
        var invoker =
                new ZLinkChannelHandlerInvoker(
                        serializer,
                        new ZLinkCodecRegistration(),
                        type -> {
                            long expected = filtered && type == ProbeHandler.class ? 0 : 1;
                            assertEquals(
                                    expected,
                                    jobs.snapshot().permitsInUse(),
                                    "activation precedes callback");
                            preparationCalls.incrementAndGet();
                            if (dispatch == Dispatch.ACTIVATION_THROW)
                                throw new IllegalStateException("activation");
                            return type == ProbeHandler.class ? handler : filter;
                        },
                        Runnable::run,
                        filtered ? List.of(filter.getClass()) : List.of());
        var executor = new ManualExecutor();
        var serial = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.generic());
        boolean expectedFailure =
                switch (dispatch) {
                    case THROW, FILTER_THROW, DECODE_THROW, ACTIVATION_THROW -> true;
                    default -> false;
                };
        var permit =
                jobs.acquire(ZLinkApplicationJobQueue.Origin.REMOTE).toCompletableFuture().join();
        try (var payload = Message.from("payload");
                var ignored = ZLinkApplicationJobContext.enter(permit)) {
            var completed =
                    serial.enqueue(
                            () ->
                                    invoker.executeHandler(
                                            () ->
                                                    invoker.invokeSendHandler(
                                                            "channel",
                                                            new ChannelSendHandlerRegistration(
                                                                    ProbeHandler.class,
                                                                    String.class,
                                                                    "packet"),
                                                            payload.dataBuffer())),
                            null);
            executor.drain();
            if (completed.toCompletableFuture().isCompletedExceptionally() && !expectedFailure) {
                completed.toCompletableFuture().join();
            }
            assertEquals(0, jobs.snapshot().permitsInUse());
            if (dispatch == Dispatch.AWAIT || dispatch == Dispatch.YIELD) {
                assertFalse(completed.toCompletableFuture().isDone());
                var nextPermit =
                        jobs.acquire(ZLinkApplicationJobQueue.Origin.REMOTE)
                                .toCompletableFuture()
                                .join();
                try {
                    continuation.complete(null);
                    executor.drain();
                    completed.toCompletableFuture().join();
                    assertEquals(
                            1,
                            jobs.snapshot().permitsInUse(),
                            "terminal cleanup cannot release next job");
                } finally {
                    nextPermit.close();
                }
            } else if (expectedFailure) {
                assertThrows(
                        java.util.concurrent.CompletionException.class,
                        () -> completed.toCompletableFuture().join());
            } else {
                completed.toCompletableFuture().join();
            }
        } finally {
            continuation.complete(null);
            permit.abandonReservation();
            serial.close();
            executor.drain();
            serial.awaitQuiescence().toCompletableFuture().join();
        }
        int expectedHandlerCalls =
                switch (dispatch) {
                    case FILTER_STOP, FILTER_THROW, DECODE_THROW, ACTIVATION_THROW -> 0;
                    default -> 1;
                };
        assertEquals(expectedHandlerCalls, handlerCalls.get());
        assertEquals(
                dispatch == Dispatch.DECODE_THROW
                        ? 1
                        : filtered && dispatch == Dispatch.FILTER_CONTINUE ? 3 : 2,
                preparationCalls.get());
        assertEquals(0, jobs.snapshot().permitsInUse());
    }

    @Test
    void shutdownRejectsDispatchAndReturnsPermitWithoutCallingHandler() {
        var jobs = queue();
        var executor = new ManualExecutor();
        var serial = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.generic());
        var calls = new AtomicInteger();
        var permit =
                jobs.acquire(ZLinkApplicationJobQueue.Origin.REMOTE).toCompletableFuture().join();
        CompletionStage<Void> result;
        executor.shutdown();
        try (var ignored = ZLinkApplicationJobContext.enter(permit)) {
            result =
                    serial.enqueue(
                            () -> {
                                calls.incrementAndGet();
                                return CompletableFuture.completedFuture(null);
                            },
                            null);
        } finally {
            permit.abandonReservation();
        }
        serial.close();
        executor.drain();
        assertThrows(
                java.util.concurrent.CompletionException.class,
                () -> result.toCompletableFuture().join());
        serial.awaitQuiescence().toCompletableFuture().join();
        assertEquals(0, calls.get());
        assertEquals(0, jobs.snapshot().permitsInUse());
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void asynchronousCompletionRestoresDispatchFlowForDownstreamObserver(boolean failed)
            throws Exception {
        var invoker =
                new ZLinkChannelHandlerInvoker(
                        new ZLinkStringMessageSerializer(),
                        new ZLinkCodecRegistration(),
                        type -> null,
                        Runnable::run,
                        List.of());
        var flow = ZLinkFlowContext.create(ZLinkFlowOrigin.INBOUND);
        var source = new CompletableFuture<Void>();
        var seen = new AtomicReference<ZLinkFlowContext.State>();
        CompletionStage<Void> observed;
        try (var ignored = ZLinkFlowContext.enter(flow)) {
            observed =
                    invoker.executeHandler(() -> source)
                            .whenComplete((value, error) -> seen.set(ZLinkFlowContext.current()));
        }
        var completion =
                Thread.ofPlatform()
                        .start(
                                () -> {
                                    if (failed)
                                        source.completeExceptionally(
                                                new IllegalStateException("async"));
                                    else source.complete(null);
                                });
        completion.join();
        if (failed)
            assertThrows(
                    java.util.concurrent.CompletionException.class,
                    () -> observed.toCompletableFuture().join());
        else observed.toCompletableFuture().join();
        assertSame(flow, seen.get());
        assertNull(ZLinkFlowContext.current());
    }

    private static final class ManualExecutor extends AbstractExecutorService {
        private final ArrayDeque<Runnable> commands = new ArrayDeque<>();
        private boolean shutdown;

        void drain() {
            while (!commands.isEmpty()) commands.removeFirst().run();
        }

        @Override
        public void execute(Runnable command) {
            if (shutdown) throw new java.util.concurrent.RejectedExecutionException("shutdown");
            commands.addLast(command);
        }

        @Override
        public void shutdown() {
            shutdown = true;
        }

        @Override
        public List<Runnable> shutdownNow() {
            throw new UnsupportedOperationException();
        }

        @Override
        public boolean isShutdown() {
            return shutdown;
        }

        @Override
        public boolean isTerminated() {
            return shutdown && commands.isEmpty();
        }

        @Override
        public boolean awaitTermination(long timeout, TimeUnit unit) {
            return isTerminated();
        }
    }

    private static ZLinkApplicationJobQueue queue() {
        return new ZLinkApplicationJobQueue(
                ZLinkApplicationJobQueueProfile.BALANCED,
                OptionalLong.of(1),
                new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null));
    }

    public static final class ProbeHandler implements ZLinkSendHandler<String> {
        private final ZLinkApplicationJobQueue jobs;
        private final Dispatch dispatch;
        private final CompletableFuture<Void> continuation;
        private final AtomicInteger calls;

        ProbeHandler(
                ZLinkApplicationJobQueue jobs,
                Dispatch dispatch,
                CompletableFuture<Void> continuation,
                AtomicInteger calls) {
            this.jobs = jobs;
            this.dispatch = dispatch;
            this.continuation = continuation;
            this.calls = calls;
        }

        @Override
        public CompletionStage<Void> handle(String message, ZLinkMessageContext context) {
            assertEquals(0, jobs.snapshot().permitsInUse());
            calls.incrementAndGet();
            if (dispatch == Dispatch.THROW) throw new IllegalStateException("handler");
            if (dispatch == Dispatch.AWAIT) return continuation;
            if (dispatch == Dispatch.YIELD)
                return ZLinkSerialExecutionQueue.yieldCurrent(continuation);
            return CompletableFuture.completedFuture(null);
        }
    }
}
