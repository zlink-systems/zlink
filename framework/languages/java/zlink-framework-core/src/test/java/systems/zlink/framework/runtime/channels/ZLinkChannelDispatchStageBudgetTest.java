package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.messaging.ZLinkStringMessageSerializer;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Function;

public final class ZLinkChannelDispatchStageBudgetTest {
    @Test
    void unfilteredRequestOnlyMapsTheReplyAndRetainsScopeCompletion() {
        var counts = new int[3];
        var handler = new Handler(counts);
        var codecs = new ZLinkCodecRegistration();
        var serializer = new ZLinkStringMessageSerializer();
        var invoker =
                new ZLinkChannelHandlerInvoker(
                        serializer, codecs, type -> handler, Runnable::run, List.of());
        for (int i = 0; i < 1000; i++) {
            try (var payload = Message.from("request");
                    var reply =
                            invoker.invokeRequestHandler(
                                            "channel",
                                            new ChannelRequestHandlerRegistration(
                                                    Handler.class,
                                                    String.class,
                                                    String.class,
                                                    "request"),
                                            payload)
                                    .toCompletableFuture()
                                    .join()) {
                assertEquals("reply", reply.toUtf8String());
            }
        }
        var baseline = new int[3];
        var baselineHandler = new Handler(baseline);
        var registration =
                new ChannelRequestHandlerRegistration(
                        Handler.class, String.class, String.class, "request");
        for (int i = 0; i < 1000; i++)
            registration
                    .invocation()
                    .invoke(baselineHandler, "request", null)
                    .toCompletableFuture()
                    .join();
        System.out.println(
                "channel thenApply="
                        + counts[1]
                        + " prepared invocation baseline="
                        + baseline[1]
                        + " thenCompose="
                        + counts[2]);
        assertEquals(1000, counts[0], "one handler invocation per request");
        assertEquals(1000, counts[1] - baseline[1], "one reply mapping per request");
        assertEquals(0, counts[2], "no asynchronous composition without a filter");
    }

    public static Runnable allocationFixture(boolean filtered) {
        return allocationFixture(filtered ? PassThroughFilter.class : null);
    }

    public static Runnable rejectingAllocationFixture() {
        return allocationFixture(RejectingFilter.class);
    }

    private static Runnable allocationFixture(
            Class<? extends systems.zlink.framework.ZLinkHandlerFilter> filterType) {
        var counts = new int[3];
        var handler = new Handler(counts);
        var serializer = new ZLinkStringMessageSerializer();
        var invoker =
                new ZLinkChannelHandlerInvoker(
                        serializer,
                        new ZLinkCodecRegistration(),
                        type ->
                                type == PassThroughFilter.class
                                        ? new PassThroughFilter()
                                        : type == RejectingFilter.class
                                                ? new RejectingFilter()
                                                : handler,
                        Runnable::run,
                        filterType == null ? List.of() : List.of(filterType));
        var registration =
                new ChannelRequestHandlerRegistration(
                        Handler.class, String.class, String.class, "request");
        return () -> {
            try (var payload = Message.from("request")) {
                var result = invoker.invokeRequestHandler("channel", registration, payload);
                if (filterType == RejectingFilter.class) {
                    var rejected =
                            org.junit.jupiter.api.Assertions.assertThrows(
                                    java.util.concurrent.CompletionException.class,
                                    () -> result.toCompletableFuture().join());
                    var error =
                            org.junit.jupiter.api.Assertions.assertInstanceOf(
                                    systems.zlink.framework.errors.ZLinkFrameworkException.class,
                                    rejected.getCause());
                    assertEquals(
                            systems.zlink.framework.errors.ZLinkFrameworkErrorKind.REJECTED,
                            error.kind());
                    assertEquals(0, counts[0]);
                } else {
                    try (var reply = result.toCompletableFuture().join()) {
                        assertEquals("reply", reply.toUtf8String());
                    }
                }
            }
        };
    }

    public static final class RejectingFilter
            implements systems.zlink.framework.ZLinkHandlerFilter {
        @Override
        public <T> CompletionStage<T> invoke(
                systems.zlink.framework.ZLinkHandlerFilterContext context,
                systems.zlink.framework.ZLinkHandlerFilterNext<T> next) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PassThroughFilter
            implements systems.zlink.framework.ZLinkHandlerFilter {
        @Override
        public <T> CompletionStage<T> invoke(
                systems.zlink.framework.ZLinkHandlerFilterContext context,
                systems.zlink.framework.ZLinkHandlerFilterNext<T> next) {
            return next.invoke();
        }
    }

    public static final class Handler implements ZLinkRequestHandler<String, String> {
        private final int[] counts;

        Handler(int[] counts) {
            this.counts = counts;
        }

        public CompletionStage<String> handle(String request, ZLinkMessageContext context) {
            counts[0]++;
            var stage = new CountedFuture<String>(counts);
            stage.complete("reply");
            return stage;
        }
    }

    private static final class CountedFuture<T> extends CompletableFuture<T> {
        private final int[] counts;

        CountedFuture(int[] counts) {
            this.counts = counts;
        }

        @Override
        public <U> CompletableFuture<U> newIncompleteFuture() {
            return new CountedFuture<>(counts);
        }

        @Override
        public <U> CompletableFuture<U> thenApply(Function<? super T, ? extends U> action) {
            counts[1]++;
            return super.thenApply(action);
        }

        @Override
        public <U> CompletableFuture<U> thenCompose(
                Function<? super T, ? extends CompletionStage<U>> action) {
            counts[2]++;
            return super.thenCompose(action);
        }
    }
}
