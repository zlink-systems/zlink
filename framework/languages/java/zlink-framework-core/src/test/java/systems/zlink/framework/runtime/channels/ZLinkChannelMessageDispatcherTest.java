package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;
import systems.zlink.framework.channels.ZLinkRouteSendHandler;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorReply;
import systems.zlink.framework.runtime.messaging.ZLinkStringMessageSerializer;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.logging.Handler;
import java.util.logging.Level;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class ZLinkChannelMessageDispatcherTest {
    private static volatile GateProbe gateProbe;

    @Test
    void admissionRejectionReleasesPayloadOnceWithoutReleasingAcceptedWork() {
        AtomicInteger releases = new AtomicInteger();
        AtomicInteger replies = new AtomicInteger();
        CompletableFuture<Void> rejected = new CompletableFuture<>();
        ZLinkChannelDispatchReporter.onAdmissionRejected(
                rejected, releases::incrementAndGet, ignored -> replies.incrementAndGet());
        rejected.completeExceptionally(new IllegalStateException("closing"));
        assertEquals(1, releases.get());
        assertEquals(1, replies.get());

        CompletableFuture<Void> admitted = new CompletableFuture<>();
        ZLinkChannelDispatchReporter.onAdmissionRejected(
                admitted, releases::incrementAndGet, ignored -> replies.incrementAndGet());
        admitted.complete(null);
        assertEquals(1, releases.get());
        assertEquals(1, replies.get());
    }

    @Test
    void clientServerSerializesSendAndRequestByChannelName() throws Exception {
        verifyChannelGate(false);
    }

    @Test
    void routeMeshSerializesSendAndRequestByChannelName() throws Exception {
        verifyChannelGate(true);
    }

    @Test
    void closingChannelRejectsNewRequestsWithShutdownReply() throws Exception {
        for (boolean route : List.of(false, true)) {
            GateProbe probe = new GateProbe();
            gateProbe = probe;
            try {
                ZLinkDispatchOptionsRegistration options = new ZLinkDispatchOptionsRegistration();
                ZLinkDispatchErrorReporter errors =
                        new ZLinkDispatchErrorReporter(
                                options, ZLinkHandlerActivator.reflection(), Runnable::run);
                ZLinkChannelDispatchRegistry registry =
                        new ZLinkChannelDispatchRegistry(Runnable::run);
                if (route) {
                    registry.registerRoute(
                            "orders",
                            Map.of(),
                            Map.of(
                                    "Request",
                                    new ChannelRouteRequestHandlerRegistration(
                                            ProbeRouteRequestHandler.class,
                                            String.class,
                                            String.class,
                                            "Request")));
                } else {
                    registry.registerClientServer(
                            "orders",
                            Map.of(),
                            Map.of(
                                    "Request",
                                    new ChannelRequestHandlerRegistration(
                                            ProbeRequestHandler.class,
                                            String.class,
                                            String.class,
                                            "Request")));
                }
                registry.sealClosingAdmission();
                ZLinkChannelDispatchReporter reporter = new ZLinkChannelDispatchReporter(errors);
                ZLinkChannelMessageDispatcher client =
                        new ZLinkChannelMessageDispatcher(
                                registry, invoker(), reporter, errors.flow());
                ZLinkChannelRouteDispatcher mesh =
                        new ZLinkChannelRouteDispatcher(
                                null,
                                registry,
                                invoker(),
                                reporter,
                                errors.flow(),
                                null,
                                ignored -> null);
                CompletableFuture<ZLinkFrameworkErrorKind> rejected = new CompletableFuture<>();
                ZLinkBackendReceived received =
                        new ZLinkBackendReceived(
                                ZLinkBackendRequestResult.OK,
                                Optional.of(RoutingId.from("client")),
                                Optional.empty(),
                                Optional.of(7L),
                                List.of(
                                        Message.from("Request".getBytes(StandardCharsets.UTF_8)),
                                        Message.from("first".getBytes(StandardCharsets.UTF_8))),
                                parts -> rejected.complete(ZLinkFrameworkErrorReply.kind(parts)),
                                () -> {});
                if (route) {
                    mesh.dispatch("orders", null, received);
                } else {
                    client.dispatchRequest("orders", null, received);
                }
                assertEquals(
                        ZLinkFrameworkErrorKind.SHUTTING_DOWN, rejected.get(2, TimeUnit.SECONDS));
                assertFalse(probe.firstEntered.isDone());
            } finally {
                gateProbe = null;
            }
        }
    }

    private static void verifyChannelGate(boolean route) throws Exception {
        GateProbe probe = new GateProbe();
        gateProbe = probe;
        try {
            ZLinkDispatchOptionsRegistration options = new ZLinkDispatchOptionsRegistration();
            ZLinkDispatchErrorReporter errors =
                    new ZLinkDispatchErrorReporter(
                            options, ZLinkHandlerActivator.reflection(), Runnable::run);
            ZLinkChannelDispatchRegistry registry = new ZLinkChannelDispatchRegistry(Runnable::run);
            for (String channel : List.of("orders", "other")) {
                if (route) {
                    registry.registerRoute(
                            channel,
                            Map.of(
                                    "Send",
                                    new ChannelRouteSendHandlerRegistration(
                                            ProbeRouteSendHandler.class, String.class, "Send")),
                            Map.of(
                                    "Request",
                                    new ChannelRouteRequestHandlerRegistration(
                                            ProbeRouteRequestHandler.class,
                                            String.class,
                                            String.class,
                                            "Request")));
                } else {
                    registry.registerClientServer(
                            channel,
                            Map.of(
                                    "Send",
                                    new ChannelSendHandlerRegistration(
                                            ProbeSendHandler.class, String.class, "Send")),
                            Map.of(
                                    "Request",
                                    new ChannelRequestHandlerRegistration(
                                            ProbeRequestHandler.class,
                                            String.class,
                                            String.class,
                                            "Request")));
                }
            }
            ZLinkChannelDispatchReporter reporter = new ZLinkChannelDispatchReporter(errors);
            ZLinkChannelMessageDispatcher client =
                    new ZLinkChannelMessageDispatcher(registry, invoker(), reporter, errors.flow());
            ZLinkChannelRouteDispatcher mesh =
                    new ZLinkChannelRouteDispatcher(
                            null,
                            registry,
                            invoker(),
                            reporter,
                            errors.flow(),
                            null,
                            ignored -> null);

            dispatch(route, client, mesh, "orders", "Request", "first", probe.firstReply);
            probe.firstEntered.get(2, TimeUnit.SECONDS);
            dispatch(route, client, mesh, "orders", "Send", "send", null);
            dispatch(route, client, mesh, "orders", "Request", "later", probe.laterReply);
            dispatch(route, client, mesh, "other", "Request", "other", probe.otherReply);
            probe.otherReply.get(2, TimeUnit.SECONDS);

            assertThrows(
                    TimeoutException.class,
                    () -> probe.sendEntered.get(250, TimeUnit.MILLISECONDS));
            assertFalse(probe.sendEntered.isDone());
            assertFalse(probe.laterEntered.isDone());
            assertTrue(probe.maximumRunning.get() <= 1);

            probe.releaseFirst.complete("first-reply");
            probe.firstReply.get(2, TimeUnit.SECONDS);
            probe.sendEntered.get(2, TimeUnit.SECONDS);
            probe.laterEntered.get(2, TimeUnit.SECONDS);
            probe.laterReply.get(2, TimeUnit.SECONDS);
            assertTrue(probe.maximumRunning.get() <= 1);
        } finally {
            probe.releaseFirst.complete("first-reply");
            gateProbe = null;
        }
    }

    private static void dispatch(
            boolean route,
            ZLinkChannelMessageDispatcher client,
            ZLinkChannelRouteDispatcher mesh,
            String channel,
            String packet,
            String body,
            CompletableFuture<String> reply) {
        ZLinkBackendReceived received =
                new ZLinkBackendReceived(
                        ZLinkBackendRequestResult.OK,
                        Optional.of(RoutingId.from("client")),
                        Optional.empty(),
                        reply == null ? Optional.empty() : Optional.of(7L),
                        List.of(
                                Message.from(packet.getBytes(StandardCharsets.UTF_8)),
                                Message.from(body.getBytes(StandardCharsets.UTF_8))),
                        reply == null
                                ? null
                                : parts -> reply.complete(parts.getLast().toUtf8String()),
                        () -> {});
        if (route) {
            mesh.dispatch(channel, null, received);
        } else {
            client.dispatchRequest(channel, null, received);
        }
    }

    private static final class GateProbe {
        private final CompletableFuture<Void> firstEntered = new CompletableFuture<>();
        private final CompletableFuture<Void> sendEntered = new CompletableFuture<>();
        private final CompletableFuture<Void> laterEntered = new CompletableFuture<>();
        private final CompletableFuture<String> releaseFirst = new CompletableFuture<>();
        private final CompletableFuture<String> firstReply = new CompletableFuture<>();
        private final CompletableFuture<String> laterReply = new CompletableFuture<>();
        private final CompletableFuture<String> otherReply = new CompletableFuture<>();
        private final AtomicInteger running = new AtomicInteger();
        private final AtomicInteger maximumRunning = new AtomicInteger();

        private CompletionStage<String> request(String value) {
            if (value.equals("other")) {
                return CompletableFuture.completedFuture("other-reply");
            }
            int active = running.incrementAndGet();
            maximumRunning.accumulateAndGet(active, Math::max);
            if (value.equals("first")) {
                firstEntered.complete(null);
                return releaseFirst.whenComplete((ignored, error) -> running.decrementAndGet());
            }
            laterEntered.complete(null);
            running.decrementAndGet();
            return CompletableFuture.completedFuture("later-reply");
        }

        private CompletionStage<Void> send() {
            int active = running.incrementAndGet();
            maximumRunning.accumulateAndGet(active, Math::max);
            sendEntered.complete(null);
            running.decrementAndGet();
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ProbeRequestHandler implements ZLinkRequestHandler<String, String> {
        @Override
        public CompletionStage<String> handle(String request, ZLinkMessageContext context) {
            return gateProbe.request(request);
        }
    }

    public static final class ProbeRouteRequestHandler
            implements ZLinkRouteRequestHandler<String, String> {
        @Override
        public CompletionStage<String> handle(String request, ZLinkRouteMessageContext context) {
            return gateProbe.request(request);
        }
    }

    public static final class ProbeSendHandler implements ZLinkSendHandler<String> {
        @Override
        public CompletionStage<Void> handle(String message, ZLinkMessageContext context) {
            return gateProbe.send();
        }
    }

    public static final class ProbeRouteSendHandler implements ZLinkRouteSendHandler<String> {
        @Override
        public CompletionStage<Void> handle(String message, ZLinkRouteMessageContext context) {
            return gateProbe.send();
        }
    }

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
            ZLinkDispatchOptionsRegistration options = new ZLinkDispatchOptionsRegistration();
            options.messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            ZLinkDispatchErrorReporter errors =
                    new ZLinkDispatchErrorReporter(
                            options, ZLinkHandlerActivator.reflection(), Runnable::run);
            ZLinkChannelDispatchRegistry registry = new ZLinkChannelDispatchRegistry(Runnable::run);
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
            ZLinkDispatchOptionsRegistration options = new ZLinkDispatchOptionsRegistration();
            options.messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            ZLinkDispatchErrorReporter errors =
                    new ZLinkDispatchErrorReporter(
                            options, ZLinkHandlerActivator.reflection(), Runnable::run);
            ZLinkChannelDispatchRegistry registry = new ZLinkChannelDispatchRegistry(Runnable::run);
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
            ZLinkDispatchOptionsRegistration options = new ZLinkDispatchOptionsRegistration();
            options.messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            ZLinkDispatchErrorReporter errors =
                    new ZLinkDispatchErrorReporter(
                            options, ZLinkHandlerActivator.reflection(), Runnable::run);
            ZLinkChannelDispatchRegistry registry = new ZLinkChannelDispatchRegistry(Runnable::run);
            registry.registerRoute("routes", Map.of(), Map.of());
            registry.registerInternalRequest(
                    "Request",
                    (source, request) ->
                            CompletableFuture.completedFuture(
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
                () -> {});
    }

    private static Handler traceHandler(
            List<String> traces, CompletableFuture<String> dispatchError) {
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
            public void flush() {}

            @Override
            public void close() {}
        };
    }

    private static void assertReplyPathMissingWithoutReplied(
            List<String> traces, CompletableFuture<String> dispatchError) throws Exception {
        assertTrue(dispatchError.get(2, TimeUnit.SECONDS).contains("action=drop"));
        assertFalse(traces.stream().anyMatch(line -> line.contains("phase=replied")));
    }

    public static final class SuccessfulRequestHandler
            implements ZLinkRequestHandler<String, String> {
        @Override
        public CompletionStage<String> handle(String request, ZLinkMessageContext context) {
            return CompletableFuture.completedFuture("reply");
        }
    }

    public static final class SuccessfulRouteRequestHandler
            implements ZLinkRouteRequestHandler<String, String> {
        @Override
        public CompletionStage<String> handle(String request, ZLinkRouteMessageContext context) {
            return CompletableFuture.completedFuture("reply");
        }
    }
}
