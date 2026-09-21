package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.logging.Handler;
import java.util.logging.Level;
import java.util.logging.LogRecord;
import java.util.logging.Logger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.messaging.ZLinkStringMessageSerializer;

final class ZLinkChannelMessageDispatcherTest {
    @Test
    void replyWriteFailureReportsMissingPathWithoutRepliedTrace() throws Exception {
        List<String> traces = new CopyOnWriteArrayList<>();
        CompletableFuture<String> dispatchError = new CompletableFuture<>();
        Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        Level previousLevel = logger.getLevel();
        Handler handler = traceHandler(traces, dispatchError);
        logger.setLevel(Level.ALL);
        logger.addHandler(handler);
        try {
            ZLinkDispatchOptionsRegistration options =
                new ZLinkDispatchOptionsRegistration();
            options.messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            ZLinkDispatchErrorReporter errors = new ZLinkDispatchErrorReporter(
                options,
                ZLinkHandlerActivator.reflection(),
                Runnable::run);
            ZLinkChannelDispatchRegistry registry =
                new ZLinkChannelDispatchRegistry(Runnable::run);
            registry.registerClientServer(
                "orders",
                Map.of(),
                Map.of(
                    "Request",
                    new ChannelRequestHandlerRegistration(
                        SuccessfulRequestHandler.class,
                        String.class,
                        String.class,
                        "Request")));
            ZLinkChannelHandlerInvoker invoker = invoker();
            ZLinkChannelMessageDispatcher dispatcher =
                new ZLinkChannelMessageDispatcher(
                    registry,
                    invoker,
                    new ZLinkChannelDispatchReporter(errors),
                    errors.flow());
            ZLinkBackendReceived received = receivedWithFailingReply();

            dispatcher.dispatchRequest("orders", null, received);

            assertReplyPathMissingWithoutReplied(traces, dispatchError);
        } finally {
            logger.removeHandler(handler);
            logger.setLevel(previousLevel);
        }
    }

    @Test
    void routeReplyWriteFailureReportsMissingPathWithoutRepliedTrace() throws Exception {
        List<String> traces = new CopyOnWriteArrayList<>();
        CompletableFuture<String> dispatchError = new CompletableFuture<>();
        Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        Level previousLevel = logger.getLevel();
        Handler handler = traceHandler(traces, dispatchError);
        logger.setLevel(Level.ALL);
        logger.addHandler(handler);
        try {
            ZLinkDispatchOptionsRegistration options =
                new ZLinkDispatchOptionsRegistration();
            options.messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            ZLinkDispatchErrorReporter errors = new ZLinkDispatchErrorReporter(
                options,
                ZLinkHandlerActivator.reflection(),
                Runnable::run);
            ZLinkChannelDispatchRegistry registry =
                new ZLinkChannelDispatchRegistry(Runnable::run);
            registry.registerRoute(
                "routes",
                Map.of(),
                Map.of(
                    "Request",
                    new ChannelRouteRequestHandlerRegistration(
                        SuccessfulRouteRequestHandler.class,
                        String.class,
                        String.class,
                        "Request")));
            ZLinkChannelHandlerInvoker invoker = invoker();
            ZLinkChannelRouteDispatcher dispatcher =
                new ZLinkChannelRouteDispatcher(
                    null,
                    registry,
                    invoker,
                    new ZLinkChannelDispatchReporter(errors),
                    errors.flow(),
                    null,
                    ignored -> null);
            ZLinkBackendReceived received = receivedWithFailingReply();

            dispatcher.dispatch("routes", null, received);

            assertReplyPathMissingWithoutReplied(traces, dispatchError);
        } finally {
            logger.removeHandler(handler);
            logger.setLevel(previousLevel);
        }
    }

    @Test
    void internalRouteReplyWriteFailureReportsMissingPathWithoutRepliedTrace() throws Exception {
        List<String> traces = new CopyOnWriteArrayList<>();
        CompletableFuture<String> dispatchError = new CompletableFuture<>();
        Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        Level previousLevel = logger.getLevel();
        Handler handler = traceHandler(traces, dispatchError);
        logger.setLevel(Level.ALL);
        logger.addHandler(handler);
        try {
            ZLinkDispatchOptionsRegistration options =
                new ZLinkDispatchOptionsRegistration();
            options.messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            ZLinkDispatchErrorReporter errors = new ZLinkDispatchErrorReporter(
                options,
                ZLinkHandlerActivator.reflection(),
                Runnable::run);
            ZLinkChannelDispatchRegistry registry =
                new ZLinkChannelDispatchRegistry(Runnable::run);
            registry.registerRoute("routes", Map.of(), Map.of());
            registry.registerInternalRequest(
                "Request",
                (source, request) -> CompletableFuture.completedFuture(
                    Message.from("reply".getBytes(StandardCharsets.UTF_8))));
            ZLinkChannelRouteDispatcher dispatcher =
                new ZLinkChannelRouteDispatcher(
                    null,
                    registry,
                    invoker(),
                    new ZLinkChannelDispatchReporter(errors),
                    errors.flow(),
                    null,
                    ignored -> null);

            dispatcher.dispatch("routes", null, receivedWithFailingReply());

            assertReplyPathMissingWithoutReplied(traces, dispatchError);
        } finally {
            logger.removeHandler(handler);
            logger.setLevel(previousLevel);
        }
    }

    private static ZLinkChannelHandlerInvoker invoker() {
        return new ZLinkChannelHandlerInvoker(
            new ZLinkStringMessageSerializer(),
            new ZLinkCodecRegistration(),
            ZLinkHandlerActivator.reflection(),
            Runnable::run,
            List.of(),
            List.of());
    }

    private static ZLinkBackendReceived receivedWithFailingReply() {
        return new ZLinkBackendReceived(
            ZLinkBackendRequestResult.OK,
            Optional.of(RoutingId.from("client")),
            Optional.empty(),
            Optional.of(7L),
            List.of(
                Message.from("Request".getBytes(StandardCharsets.UTF_8)),
                Message.from("request".getBytes(StandardCharsets.UTF_8))),
            ignored -> {
                throw new IllegalStateException("reply path closed");
            },
            () -> { });
    }

    private static Handler traceHandler(
        List<String> traces,
        CompletableFuture<String> dispatchError) {
        return new Handler() {
            @Override
            public void publish(LogRecord record) {
                String message = record.getMessage();
                traces.add(message);
                if (message.contains("event_id=zlink.dispatch_error")
                    && message.contains("reason=reply_path_missing")) {
                    dispatchError.complete(message);
                }
            }

            @Override
            public void flush() {
            }

            @Override
            public void close() {
            }
        };
    }

    private static void assertReplyPathMissingWithoutReplied(
        List<String> traces,
        CompletableFuture<String> dispatchError) throws Exception {
        assertTrue(dispatchError.get(2, TimeUnit.SECONDS).contains("action=drop"));
        assertFalse(traces.stream().anyMatch(line -> line.contains("phase=replied")));
    }

    public static final class SuccessfulRequestHandler
        implements ZLinkRequestHandler<String, String> {
        @Override
        public CompletionStage<String> handle(
            String request,
            ZLinkMessageContext context) {
            return CompletableFuture.completedFuture("reply");
        }
    }

    public static final class SuccessfulRouteRequestHandler
        implements ZLinkRouteRequestHandler<String, String> {
        @Override
        public CompletionStage<String> handle(
            String request,
            ZLinkRouteMessageContext context) {
            return CompletableFuture.completedFuture("reply");
        }
    }
}
