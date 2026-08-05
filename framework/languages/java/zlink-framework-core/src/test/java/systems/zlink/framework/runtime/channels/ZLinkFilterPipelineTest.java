package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.ZLinkHandlerDispatchKind;
import systems.zlink.framework.ZLinkHandlerFilter;
import systems.zlink.framework.ZLinkHandlerFilterContext;
import systems.zlink.framework.ZLinkHandlerFilterNext;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner;

final class ZLinkFilterPipelineTest {
    @Test
    void exposesFilterOnlyDispatchContextAndPreservesHandlerReply() {
        CapturingFilter filter = new CapturingFilter();
        try (var owner = owner(filter)) {
            ZLinkFilterPipeline.Result<String> result = ZLinkFilterPipeline.invoke(
                    java.util.List.of(CapturingFilter.class),
                    owner,
                    filterContext(ZLinkHandlerDispatchKind.CHANNEL_REQUEST),
                    () -> CompletableFuture.completedFuture("handler"))
                .toCompletableFuture()
                .join();

            assertTrue(result.handlerInvoked());
            assertEquals("handler", result.value());
            assertEquals(
                ZLinkHandlerDispatchKind.CHANNEL_REQUEST,
                filter.dispatchKind);
            assertEquals("mesh-a", filter.meshName);
        }
    }

    @Test
    void ignoresFilterReplyReplacement() {
        ReplacingFilter filter = new ReplacingFilter();
        try (var owner = owner(filter)) {
            ZLinkFilterPipeline.Result<String> result = ZLinkFilterPipeline.invoke(
                    java.util.List.of(ReplacingFilter.class),
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
            ZLinkFilterPipeline.Result<String> result = ZLinkFilterPipeline.invoke(
                    java.util.List.of(ShortCircuitFilter.class),
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
                () -> ZLinkFilterPipeline.invoke(
                    java.util.List.of(DuplicateNextFilter.class),
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
        ZLinkHandlerActivator activator = type -> {
            if (type == DuplicateNextFilter.class) return outer;
            if (type == ShortCircuitFilter.class) return inner;
            throw new IllegalArgumentException("unexpected type: " + type);
        };
        try (var owner = new ZLinkHandlerInstanceOwner(activator)) {
            assertThrows(
                IllegalStateException.class,
                () -> ZLinkFilterPipeline.invoke(
                    java.util.List.of(
                        DuplicateNextFilter.class,
                        ShortCircuitFilter.class),
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

    private static ZLinkHandlerFilterContext filterContext(
        ZLinkHandlerDispatchKind kind) {
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
            ZLinkHandlerFilterContext context,
            ZLinkHandlerFilterNext<T> next) {
            dispatchKind = context.dispatchKind();
            meshName = context.meshName().orElse(null);
            return next.invoke();
        }
    }

    private static final class ReplacingFilter implements ZLinkHandlerFilter {
        @Override
        public <T> CompletionStage<T> invoke(
            ZLinkHandlerFilterContext context,
            ZLinkHandlerFilterNext<T> next) {
            return next.invoke().thenApply(ignored -> {
                @SuppressWarnings("unchecked")
                T replacement = (T) "replacement";
                return replacement;
            });
        }
    }

    private static final class ShortCircuitFilter implements ZLinkHandlerFilter {
        @Override
        public <T> CompletionStage<T> invoke(
            ZLinkHandlerFilterContext context,
            ZLinkHandlerFilterNext<T> next) {
            @SuppressWarnings("unchecked")
            T replacement = (T) "replacement";
            return CompletableFuture.completedFuture(replacement);
        }
    }

    private static final class DuplicateNextFilter implements ZLinkHandlerFilter {
        @Override
        public <T> CompletionStage<T> invoke(
            ZLinkHandlerFilterContext context,
            ZLinkHandlerFilterNext<T> next) {
            CompletionStage<T> first = next.invoke();
            next.invoke();
            return first;
        }
    }
}
