package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.ZLinkHandlerDispatchKind;
import systems.zlink.framework.ZLinkHandlerFilter;
import systems.zlink.framework.ZLinkHandlerFilterContext;
import systems.zlink.framework.ZLinkHandlerFilterNext;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner;

import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.OptionalLong;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkFilterPipelineTest {
    @ParameterizedTest
    @ValueSource(strings = {"no-filter", "pass", "reject", "sync-throw", "failed-stage"})
    void retainsQueuedPermitThroughFiltersUntilHandlerEntryOrTerminalCleanup(String outcome)
            throws Exception {
        var queue =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(1),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null));
        var first =
                queue.acquire(ZLinkApplicationJobQueue.Origin.REMOTE).toCompletableFuture().join();
        ZLinkApplicationJobContext.QueuedOwnership ownership;
        try (var ingress = ZLinkApplicationJobContext.enter(first)) {
            ownership = ZLinkApplicationJobContext.transferToQueuedJob();
        }
        var queued = queue.snapshot();
        var second = queue.acquire(ZLinkApplicationJobQueue.Origin.REMOTE).toCompletableFuture();
        boolean secondInitiallyCompleted = second.isDone();
        var handler = new PermitBoundaryHandler(queue, second);
        var invocation =
                ZLinkHandlerMethodInvoker.prepare(
                        PermitBoundaryHandler.class.getMethod("handle"), List.of());
        var sentinel = new IllegalStateException("filter failure");
        var filterEntry = new AtomicReference<ZLinkApplicationJobQueue.Snapshot>();
        ZLinkHandlerFilter filter =
                new ZLinkHandlerFilter() {
                    @Override
                    public <T> CompletionStage<T> invoke(
                            ZLinkHandlerFilterContext context, ZLinkHandlerFilterNext<T> next) {
                        filterEntry.set(queue.snapshot());
                        return switch (outcome) {
                            case "pass" -> next.invoke();
                            case "reject" -> CompletableFuture.completedFuture(null);
                            case "sync-throw" -> throw sentinel;
                            case "failed-stage" -> CompletableFuture.failedFuture(sentinel);
                            default ->
                                    throw new IllegalStateException("unexpected filter invocation");
                        };
                    }
                };
        ZLinkFilterPipeline.Result<Object> result = null;
        RuntimeException failure = null;
        ZLinkApplicationJobQueue.Snapshot terminal;
        boolean secondCompletedAtTerminal;
        try (var handlers = owner(filter);
                var dispatch = ZLinkApplicationJobContext.enterQueued(ownership)) {
            try {
                result =
                        ZLinkFilterPipeline.invoke(
                                        outcome.equals("no-filter")
                                                ? List.of()
                                                : List.of(filter.getClass()),
                                        handlers,
                                        filterContext(ZLinkHandlerDispatchKind.CHANNEL_REQUEST),
                                        () -> invocation.invoke(handler))
                                .toCompletableFuture()
                                .join();
            } catch (RuntimeException thrown) {
                failure = thrown;
            } finally {
                terminal = queue.snapshot();
                secondCompletedAtTerminal = second.isDone();
            }
        } finally {
            ownership.close();
        }
        var afterCleanup = queue.snapshot();
        var secondPermit = second.getNow(null);
        ownership.close();
        var afterRepeatedCleanup = queue.snapshot();
        if (secondPermit != null) secondPermit.close();
        else second.cancel(false);
        var drained = queue.snapshot();

        assertEquals(1, queued.queuedApplicationJobs());
        assertEquals(1, queued.permitsInUse());
        assertFalse(secondInitiallyCompleted);
        if (outcome.equals("no-filter")) assertNull(filterEntry.get());
        else {
            assertEquals(1, filterEntry.get().queuedApplicationJobs());
            assertEquals(1, filterEntry.get().permitsInUse());
        }
        boolean handlerEntered = outcome.equals("no-filter") || outcome.equals("pass");
        assertEquals(handlerEntered ? 1 : 0, handler.calls);
        if (handlerEntered) {
            assertEquals(0, handler.entry.queuedApplicationJobs());
            assertEquals(1, handler.entry.permitsInUse());
            assertTrue(handler.secondCompletedAtEntry);
            assertNull(failure);
            assertTrue(result.handlerInvoked());
            assertEquals("handler", result.value());
        } else {
            assertNull(handler.entry);
            assertEquals(1, terminal.queuedApplicationJobs());
            assertEquals(1, terminal.permitsInUse());
            assertFalse(secondCompletedAtTerminal);
            if (outcome.equals("reject")) {
                assertNull(failure);
                assertFalse(result.handlerInvoked());
            } else {
                assertNull(result);
                assertSame(
                        sentinel,
                        failure instanceof CompletionException ? failure.getCause() : failure);
            }
        }
        assertNotNull(secondPermit);
        assertEquals(0, afterCleanup.queuedApplicationJobs());
        assertEquals(1, afterCleanup.permitsInUse());
        assertEquals(0, afterRepeatedCleanup.queuedApplicationJobs());
        assertEquals(1, afterRepeatedCleanup.permitsInUse());
        assertEquals(0, drained.queuedApplicationJobs());
        assertEquals(0, drained.permitsInUse());
    }

    public static final class PermitBoundaryHandler {
        private final ZLinkApplicationJobQueue queue;
        private final CompletableFuture<ZLinkApplicationJobQueue.Permit> second;
        private ZLinkApplicationJobQueue.Snapshot entry;
        private boolean secondCompletedAtEntry;
        private int calls;

        PermitBoundaryHandler(
                ZLinkApplicationJobQueue queue,
                CompletableFuture<ZLinkApplicationJobQueue.Permit> second) {
            this.queue = queue;
            this.second = second;
        }

        public CompletionStage<String> handle() {
            entry = queue.snapshot();
            secondCompletedAtEntry = second.isDone();
            calls++;
            return CompletableFuture.completedFuture("handler");
        }
    }

    @Test
    void exposesFilterOnlyDispatchContextAndPreservesHandlerReply() {
        CapturingFilter filter = new CapturingFilter();
        try (var owner = owner(filter)) {
            ZLinkFilterPipeline.Result<String> result =
                    ZLinkFilterPipeline.invoke(
                                    List.of(CapturingFilter.class),
                                    owner,
                                    filterContext(ZLinkHandlerDispatchKind.CHANNEL_REQUEST),
                                    () -> CompletableFuture.completedFuture("handler"))
                            .toCompletableFuture()
                            .join();

            assertTrue(result.handlerInvoked());
            assertEquals("handler", result.value());
            assertEquals(ZLinkHandlerDispatchKind.CHANNEL_REQUEST, filter.dispatchKind);
            assertEquals("mesh-a", filter.meshName);
        }
    }

    @Test
    void ignoresFilterReplyReplacement() {
        ReplacingFilter filter = new ReplacingFilter();
        try (var owner = owner(filter)) {
            ZLinkFilterPipeline.Result<String> result =
                    ZLinkFilterPipeline.invoke(
                                    List.of(ReplacingFilter.class),
                                    owner,
                                    filterContext(ZLinkHandlerDispatchKind.CHANNEL_REQUEST),
                                    () -> CompletableFuture.completedFuture("handler"))
                            .toCompletableFuture()
                            .join();

            assertTrue(result.handlerInvoked());
            assertEquals("handler", result.value());
        }
    }

    @Test
    void reportsWhenFilterDoesNotInvokeNext() {
        ShortCircuitFilter filter = new ShortCircuitFilter();
        try (var owner = owner(filter)) {
            ZLinkFilterPipeline.Result<String> result =
                    ZLinkFilterPipeline.invoke(
                                    List.of(ShortCircuitFilter.class),
                                    owner,
                                    filterContext(ZLinkHandlerDispatchKind.NODE_DIRECT_REQUEST),
                                    () -> CompletableFuture.completedFuture("handler"))
                            .toCompletableFuture()
                            .join();

            assertFalse(result.handlerInvoked());
            assertEquals(null, result.value());
        }
    }

    @Test
    void atomicallyRejectsDuplicateNextWithoutRunningHandlerTwice() {
        AtomicInteger handlerCalls = new AtomicInteger();
        DuplicateNextFilter filter = new DuplicateNextFilter();
        try (var owner = owner(filter)) {
            assertThrows(
                    IllegalStateException.class,
                    () ->
                            ZLinkFilterPipeline.invoke(
                                    List.of(DuplicateNextFilter.class),
                                    owner,
                                    filterContext(ZLinkHandlerDispatchKind.NODE_DIRECT_REQUEST),
                                    () -> {
                                        handlerCalls.incrementAndGet();
                                        return CompletableFuture.completedFuture("handler");
                                    }));
        }
        assertEquals(1, handlerCalls.get());
    }

    @Test
    void rejectsDuplicateOuterNextWhenInnerFilterStopsDispatch() {
        AtomicInteger handlerCalls = new AtomicInteger();
        DuplicateNextFilter outer = new DuplicateNextFilter();
        ShortCircuitFilter inner = new ShortCircuitFilter();
        ZLinkHandlerActivator activator =
                type -> {
                    if (type == DuplicateNextFilter.class) return outer;
                    if (type == ShortCircuitFilter.class) return inner;
                    throw new IllegalArgumentException("unexpected type: " + type);
                };
        try (var owner = new ZLinkHandlerInstanceOwner(activator)) {
            assertThrows(
                    IllegalStateException.class,
                    () ->
                            ZLinkFilterPipeline.invoke(
                                    List.of(DuplicateNextFilter.class, ShortCircuitFilter.class),
                                    owner,
                                    filterContext(ZLinkHandlerDispatchKind.CLASSIC_FANOUT),
                                    () -> {
                                        handlerCalls.incrementAndGet();
                                        return CompletableFuture.completedFuture("handler");
                                    }));
        }
        assertEquals(0, handlerCalls.get());
    }

    private static ZLinkHandlerInstanceOwner owner(ZLinkHandlerFilter filter) {
        ZLinkHandlerActivator activator = ignored -> filter;
        return new ZLinkHandlerInstanceOwner(activator);
    }

    private static ZLinkHandlerFilterContext filterContext(ZLinkHandlerDispatchKind kind) {
        return new ZLinkHandlerFilterContext() {
            @Override
            public Optional<String> meshName() {
                return Optional.of("mesh-a");
            }

            @Override
            public Optional<String> channelName() {
                return Optional.of("channel-a");
            }

            @Override
            public String packetName() {
                return "packet-a";
            }

            @Override
            public Optional<String> contentType() {
                return Optional.of("application/json");
            }

            @Override
            public Map<String, String> metadata() {
                return Map.of("tenant", "alpha");
            }

            @Override
            public Optional<String> correlationId() {
                return Optional.of("correlation-a");
            }

            @Override
            public ZLinkHandlerDispatchKind dispatchKind() {
                return kind;
            }
        };
    }

    private static final class CapturingFilter implements ZLinkHandlerFilter {
        private ZLinkHandlerDispatchKind dispatchKind;
        private String meshName;

        @Override
        public <T> CompletionStage<T> invoke(
                ZLinkHandlerFilterContext context, ZLinkHandlerFilterNext<T> next) {
            dispatchKind = context.dispatchKind();
            meshName = context.meshName().orElse(null);
            return next.invoke();
        }
    }

    private static final class ReplacingFilter implements ZLinkHandlerFilter {
        @Override
        public <T> CompletionStage<T> invoke(
                ZLinkHandlerFilterContext context, ZLinkHandlerFilterNext<T> next) {
            return next.invoke()
                    .thenApply(
                            ignored -> {
                                @SuppressWarnings("unchecked")
                                T replacement = (T) "replacement";
                                return replacement;
                            });
        }
    }

    private static final class ShortCircuitFilter implements ZLinkHandlerFilter {
        @Override
        public <T> CompletionStage<T> invoke(
                ZLinkHandlerFilterContext context, ZLinkHandlerFilterNext<T> next) {
            @SuppressWarnings("unchecked")
            T replacement = (T) "replacement";
            return CompletableFuture.completedFuture(replacement);
        }
    }

    private static final class DuplicateNextFilter implements ZLinkHandlerFilter {
        @Override
        public <T> CompletionStage<T> invoke(
                ZLinkHandlerFilterContext context, ZLinkHandlerFilterNext<T> next) {
            CompletionStage<T> first = next.invoke();
            next.invoke();
            return first;
        }
    }
}
