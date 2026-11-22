package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
import systems.zlink.framework.runtime.messaging.ZLinkStringMessageSerializer;

import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;

/** The node-direct reply is serialized by one step applied to the handler's own result stage. */
final class ZLinkNodeDirectReplyStageTest {
    private static final AtomicInteger THEN_APPLY = new AtomicInteger();

    @Test
    void handlerResultStageIsMappedOnceWithTheSerializer() {
        THEN_APPLY.set(0);
        var invoker =
                new ZLinkChannelHandlerInvoker(
                        new ZLinkStringMessageSerializer(),
                        new ZLinkCodecRegistration(),
                        ZLinkHandlerActivator.reflection(),
                        Runnable::run,
                        List.of());
        var registration =
                new ChannelRouteRequestHandlerRegistration(
                        SpyReplyHandler.class, String.class, String.class, "Request");
        try (Message payload = Message.from("ping".getBytes(StandardCharsets.UTF_8))) {
            var reply =
                    invoker.invokeRouteRequestHandler(
                                    "orders",
                                    registration,
                                    RoutingId.from("source"),
                                    payload.dataBuffer(),
                                    Map.of())
                            .toCompletableFuture()
                            .join();
            try {
                var buffer = ZLinkFrameworkMultipartView.buffer(reply, 0);
                byte[] bytes = new byte[buffer.remaining()];
                buffer.get(bytes);
                assertEquals("ping-reply", new String(bytes, StandardCharsets.UTF_8));
            } finally {
                ZLinkFrameworkMultipartView.closeAll(reply);
            }
        }
        assertEquals(
                1,
                THEN_APPLY.get(),
                "the serializer is the only stage applied to the handler's result");
    }

    @SuppressWarnings("unchecked")
    private static <T> CompletionStage<T> counting(CompletableFuture<T> delegate) {
        return (CompletionStage<T>)
                Proxy.newProxyInstance(
                        ZLinkNodeDirectReplyStageTest.class.getClassLoader(),
                        new Class<?>[] {CompletionStage.class},
                        (proxy, method, arguments) -> {
                            if (method.getName().equals("toCompletableFuture")) return delegate;
                            Object result = method.invoke(delegate, arguments);
                            if (method.getName().equals("thenApply")) {
                                THEN_APPLY.incrementAndGet();
                                return counting((CompletableFuture<Object>) result);
                            }
                            return result instanceof CompletableFuture<?> future
                                    ? counting((CompletableFuture<Object>) future)
                                    : result;
                        });
    }

    public static final class SpyReplyHandler implements ZLinkRouteRequestHandler<String, String> {
        public SpyReplyHandler() {}

        @Override
        public CompletionStage<String> handle(String message, ZLinkRouteMessageContext context) {
            return counting(CompletableFuture.completedFuture(message + "-reply"));
        }
    }
}
