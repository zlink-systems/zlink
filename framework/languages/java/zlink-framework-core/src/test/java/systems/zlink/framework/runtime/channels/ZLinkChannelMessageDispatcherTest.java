package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
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
    void clientServerStartsNextBeforePriorTerminal() throws Exception {
        verifyChannelGate(false);
    }

    @Test
    void routeMeshStartsNextBeforePriorTerminal() throws Exception {
        verifyChannelGate(true);
    }

    @org.junit.jupiter.params.ParameterizedTest
    @org.junit.jupiter.params.provider.CsvSource({
        "false,false,false,false", "false,false,true,false", "false,true,false,false",
                "false,true,true,false",
        "true,false,false,false", "true,false,true,false", "true,true,false,false",
                "true,true,true,false",
        "false,false,false,true", "false,false,true,true", "false,true,false,true",
                "false,true,true,true",
        "true,false,false,true", "true,false,true,true", "true,true,false,true",
                "true,true,true,true"
    })
    void channelStartsInFifoOrderAndCompletesIndependently(
            boolean route, boolean firstRequest, boolean secondRequest, boolean nextBatch)
            throws Exception {
        GateProbe probe = new GateProbe();
        var activations = new ZLinkTrackingHandlerActivator();
        gateProbe = probe;
        var workers = new ZLinkManualDispatchExecutor();
        var registry = new ZLinkChannelDispatchRegistry(workers);
        if (route) {
            registry.registerRoute(
                    "orders",
                    Map.of(
                            "Send",
                            new ChannelRouteSendHandlerRegistration(
                                    ProbeRouteSendHandler.class, String.class, "Send", List.of())),
                    Map.of(
                            "Request",
                            new ChannelRouteRequestHandlerRegistration(
                                    ProbeRouteRequestHandler.class,
                                    String.class,
                                    String.class,
                                    "Request",
                                    List.of())));
        } else {
            registry.registerClientServer(
                    "orders",
                    Map.of(
                            "Send",
                            new ChannelSendHandlerRegistration(
                                    ProbeSendHandler.class, String.class, "Send", List.of())),
                    Map.of(
                            "Request",
                            new ChannelRequestHandlerRegistration(
                                    ProbeRequestHandler.class,
                                    String.class,
                                    String.class,
                                    "Request",
                                    List.of())));
        }
        var errors =
                new ZLinkDispatchErrorReporter(
                        new ZLinkDispatchOptionsRegistration(),
                        ZLinkHandlerActivator.reflection(),
                        Runnable::run);
        var reporter = new ZLinkChannelDispatchReporter(errors);
        var invoker = invoker(activations, List.of());
        var client = new ZLinkChannelMessageDispatcher(registry, invoker, reporter, errors.flow());
        var mesh =
                new ZLinkChannelRouteDispatcher(
                        null, registry, invoker, reporter, errors.flow(), null, ignored -> null);
        try {
            dispatch(
                    route,
                    client,
                    mesh,
                    "orders",
                    firstRequest ? "Request" : "Send",
                    "first",
                    firstRequest ? probe.firstReply : null);
            if (nextBatch) {
                workers.drain();
                probe.firstEntered.get(2, TimeUnit.SECONDS);
            }
            dispatch(
                    route,
                    client,
                    mesh,
                    "orders",
                    secondRequest ? "Request" : "Send",
                    "second",
                    secondRequest ? probe.laterReply : null);
            workers.drain();
            probe.laterEntered.get(2, TimeUnit.SECONDS);
            if (secondRequest) probe.laterReply.get(2, TimeUnit.SECONDS);
            assertEquals(List.of("first", "second"), probe.starts);
            assertEquals(List.of("second"), probe.completions);
            assertFalse(probe.releaseFirst.isDone());
            assertFalse(probe.firstReply.isDone());
            assertEquals(2, activations.opened.get());
            assertEquals(1, activations.closed.get());
            registry.sealClosingAdmission();
            var closing = registry.awaitQuiescence().toCompletableFuture();
            assertFalse(closing.isDone());
            probe.releaseFirst.complete("first-reply");
            workers.drain();
            closing.get(2, TimeUnit.SECONDS);
            if (firstRequest) probe.firstReply.get(2, TimeUnit.SECONDS);
            assertEquals(List.of("second", "first"), probe.completions);
            assertEquals(0, probe.running.get());
            assertEquals(2, activations.closed.get());
        } finally {
            probe.releaseFirst.complete("first-reply");
            workers.drain();
            gateProbe = null;
        }
    }

    @org.junit.jupiter.params.ParameterizedTest
    @org.junit.jupiter.params.provider.ValueSource(booleans = {false, true})
    void asyncFilterEntryIsFifoWhileBodyEntryMayOvertake(boolean route) throws Exception {
        var probe = new GateProbe();
        gateProbe = probe;
        var workers = new ZLinkManualDispatchExecutor();
        var activations = new ZLinkTrackingHandlerActivator();
        var registry = requestRegistry(route, workers);
        var errors =
                new ZLinkDispatchErrorReporter(
                        new ZLinkDispatchOptionsRegistration(), activations, Runnable::run);
        var reporter = new ZLinkChannelDispatchReporter(errors);
        var invoker = invoker(activations, List.of(WaitingFilter.class));
        var client = new ZLinkChannelMessageDispatcher(registry, invoker, reporter, errors.flow());
        var mesh =
                new ZLinkChannelRouteDispatcher(
                        null, registry, invoker, reporter, errors.flow(), null, ignored -> null);
        try {
            dispatch(route, client, mesh, "orders", "Request", "first", probe.firstReply);
            dispatch(route, client, mesh, "orders", "Request", "second", probe.laterReply);
            workers.drain();
            probe.laterReply.get(2, TimeUnit.SECONDS);
            assertEquals(List.of(1, 2), probe.filterStarts);
            assertEquals(List.of("second"), probe.starts);
            assertEquals(1, activations.closed.get());
            assertFalse(probe.firstEntered.isDone());
            registry.sealClosingAdmission();
            var closing = registry.awaitQuiescence().toCompletableFuture();
            assertFalse(closing.isDone());
            probe.filterRelease.complete(null);
            probe.firstEntered.get(2, TimeUnit.SECONDS);
            assertEquals(List.of("second", "first"), probe.starts);
            assertFalse(closing.isDone());
            probe.releaseFirst.complete("first-reply");
            closing.get(2, TimeUnit.SECONDS);
            assertEquals(2, activations.closed.get());
        } finally {
            probe.filterRelease.complete(null);
            probe.releaseFirst.complete("first-reply");
            workers.drain();
            gateProbe = null;
        }
    }

    @org.junit.jupiter.params.ParameterizedTest
    @org.junit.jupiter.params.provider.CsvSource({
        "false,false",
        "false,true",
        "true,false",
        "true,true"
    })
    void terminalFailureRemainsObservedAfterLaterRecordCompletes(boolean route, boolean request)
            throws Exception {
        var probe = new GateProbe();
        gateProbe = probe;
        var workers = new ZLinkManualDispatchExecutor();
        var activations = new ZLinkTrackingHandlerActivator();
        var registry = requestRegistry(route, workers);
        var options = new ZLinkDispatchOptionsRegistration();
        options.messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        var errors = new ZLinkDispatchErrorReporter(options, activations, Runnable::run);
        var reporter = new ZLinkChannelDispatchReporter(errors);
        var invoker = invoker(activations, List.of());
        var client = new ZLinkChannelMessageDispatcher(registry, invoker, reporter, errors.flow());
        var mesh =
                new ZLinkChannelRouteDispatcher(
                        null, registry, invoker, reporter, errors.flow(), null, ignored -> null);
        var reported = new CompletableFuture<String>();
        var logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        var previous = logger.getLevel();
        var trace =
                new Handler() {
                    @Override
                    public void publish(LogRecord record) {
                        if (record.getMessage().contains("error_message=terminal-probe-failure"))
                            reported.complete(record.getMessage());
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        logger.setLevel(Level.ALL);
        logger.addHandler(trace);
        try {
            dispatch(
                    route,
                    client,
                    mesh,
                    "orders",
                    request ? "Request" : "Send",
                    "first",
                    request ? probe.firstReply : null);
            dispatch(route, client, mesh, "orders", "Request", "second", probe.laterReply);
            workers.drain();
            probe.laterReply.get(2, TimeUnit.SECONDS);
            assertFalse(reported.isDone());
            assertFalse(probe.firstReply.isDone());
            assertEquals(1, activations.closed.get());
            registry.sealClosingAdmission();
            var closing = registry.awaitQuiescence().toCompletableFuture();
            assertFalse(closing.isDone());
            probe.releaseFirst.completeExceptionally(
                    new IllegalStateException("terminal-probe-failure"));
            var failure = reported.get(2, TimeUnit.SECONDS);
            assertTrue(failure.contains("reason=handler_exception"), failure);
            closing.get(2, TimeUnit.SECONDS);
            assertEquals(2, activations.closed.get());
            assertEquals(List.of("second", "first"), probe.completions);
            if (request)
                assertEquals("error:INTERNAL_FAILURE", probe.firstReply.get(2, TimeUnit.SECONDS));
        } finally {
            probe.releaseFirst.complete("first-reply");
            workers.drain();
            logger.removeHandler(trace);
            logger.setLevel(previous);
            gateProbe = null;
        }
    }

    private static ZLinkChannelDispatchRegistry requestRegistry(
            boolean route, java.util.concurrent.Executor executor) {
        var registry = new ZLinkChannelDispatchRegistry(executor);
        if (route)
            registry.registerRoute(
                    "orders",
                    Map.of(
                            "Send",
                            new ChannelRouteSendHandlerRegistration(
                                    ProbeRouteSendHandler.class, String.class, "Send", List.of())),
                    Map.of(
                            "Request",
                            new ChannelRouteRequestHandlerRegistration(
                                    ProbeRouteRequestHandler.class,
                                    String.class,
                                    String.class,
                                    "Request",
                                    List.of())));
        else
            registry.registerClientServer(
                    "orders",
                    Map.of(
                            "Send",
                            new ChannelSendHandlerRegistration(
                                    ProbeSendHandler.class, String.class, "Send", List.of())),
                    Map.of(
                            "Request",
                            new ChannelRequestHandlerRegistration(
                                    ProbeRequestHandler.class,
                                    String.class,
                                    String.class,
                                    "Request",
                                    List.of())));
        return registry;
    }

    public static final class WaitingFilter implements systems.zlink.framework.ZLinkHandlerFilter {
        @Override
        public <T> CompletionStage<T> invoke(
                systems.zlink.framework.ZLinkHandlerFilterContext context,
                systems.zlink.framework.ZLinkHandlerFilterNext<T> next) {
            var probe = gateProbe;
            int ordinal = probe.filterStarts.size() + 1;
            probe.filterStarts.add(ordinal);
            return ordinal == 1
                    ? probe.filterRelease.thenCompose(ignored -> next.invoke())
                    : next.invoke();
        }
    }

    @org.junit.jupiter.params.ParameterizedTest
    @org.junit.jupiter.params.provider.ValueSource(booleans = {false, true})
    void fanoutStartsNextBeforePriorTerminalAndClosesBothScopes(boolean nextBatch)
            throws Exception {
        var probe = new GateProbe();
        gateProbe = probe;
        var workers = new ZLinkManualDispatchExecutor();
        var activations = new ZLinkTrackingHandlerActivator();
        var registry = new ZLinkChannelDispatchRegistry(workers);
        registry.registerFanout(
                "orders",
                Map.of(
                        "Publish",
                        new ChannelPublishHandlerRegistration(
                                ProbeSubscriber.class, String.class, "Publish")));
        var errors =
                new ZLinkDispatchErrorReporter(
                        new ZLinkDispatchOptionsRegistration(), activations, Runnable::run);
        var dispatcher =
                new ZLinkChannelMessageDispatcher(
                        registry,
                        invoker(activations, List.of()),
                        new ZLinkChannelDispatchReporter(errors),
                        errors.flow());
        try {
            publish(dispatcher, "first");
            if (nextBatch) {
                workers.drain();
                probe.firstEntered.get(2, TimeUnit.SECONDS);
            }
            publish(dispatcher, "second");
            workers.drain();
            probe.laterEntered.get(2, TimeUnit.SECONDS);
            assertEquals(List.of("first", "second"), probe.starts);
            assertEquals(List.of("second"), probe.completions);
            assertEquals(2, activations.opened.get());
            assertEquals(1, activations.closed.get());
            assertEquals(List.of("second"), probe.destroyed);
            assertFalse(probe.releaseFirst.isDone());
            registry.sealClosingAdmission();
            var stopping = registry.awaitQuiescence().toCompletableFuture();
            assertFalse(stopping.isDone());
            probe.releaseFirst.complete("first-reply");
            workers.drain();
            stopping.get(2, TimeUnit.SECONDS);
            assertEquals(List.of("second", "first"), probe.completions);
            assertEquals(List.of("second", "first"), probe.destroyed);
            assertEquals(2, activations.closed.get());
        } finally {
            probe.releaseFirst.complete("first-reply");
            workers.drain();
            gateProbe = null;
        }
    }

    private static void publish(ZLinkChannelMessageDispatcher dispatcher, String value) {
        dispatcher.dispatchPublish(
                "orders",
                new systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage(
                        Optional.empty(),
                        "updates",
                        List.of(Message.from("Publish"), Message.from(value))));
    }

    public static final class ProbeSubscriber implements AutoCloseable {
        private String value;

        public CompletionStage<Void> handle(
                String message,
                systems.zlink.framework.channels.ZLinkPublishMessageContext context) {
            value = message;
            assertEquals("orders", context.channelName().orElseThrow());
            return gateProbe.request(message).thenApply(ignored -> null);
        }

        @Override
        public void close() {
            gateProbe.destroyed.add(value);
        }
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
                                            "Request",
                                            java.util.List.of())));
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
                                            "Request",
                                            java.util.List.of())));
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

    @Test
    void disabledTypedChannelInvokerAllocationProbe() {
        var bean =
                (com.sun.management.ThreadMXBean)
                        java.lang.management.ManagementFactory.getThreadMXBean();
        bean.setThreadAllocatedMemoryEnabled(true);
        var registration =
                new ChannelSendHandlerRegistration(
                        AllocationSendHandler.class, String.class, "Send", List.of());
        var invoker = invoker();
        AllocationSendHandler.calls = 0;
        try (var payload = Message.from("send".getBytes(StandardCharsets.UTF_8))) {
            for (int index = 0; index < 20000; index++)
                invoker.invokeSendHandler("orders", registration, payload.dataBuffer())
                        .toCompletableFuture()
                        .join();
            long thread = Thread.currentThread().threadId();
            long before = bean.getThreadAllocatedBytes(thread);
            for (int index = 0; index < 20000; index++)
                invoker.invokeSendHandler("orders", registration, payload.dataBuffer())
                        .toCompletableFuture()
                        .join();
            long allocated = bean.getThreadAllocatedBytes(thread) - before;
            assertEquals(40000, AllocationSendHandler.calls);
            assertTrue(allocated > 0L);
            System.out.println(
                    "Off typed Channel invoker allocation: " + allocated + " bytes / 20000 sends.");
        }
    }

    public static final class AllocationSendHandler implements ZLinkSendHandler<String> {
        private static int calls;

        @Override
        public CompletionStage<Void> handle(String message, ZLinkMessageContext context) {
            calls++;
            return CompletableFuture.completedFuture(null);
        }
    }

    private static void verifyChannelGate(boolean route) throws Exception {
        GateProbe probe = new GateProbe();
        gateProbe = probe;
        List<String> traces = new CopyOnWriteArrayList<>();
        Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        Level previousLevel = logger.getLevel();
        var terminals = new java.util.concurrent.CountDownLatch(4);
        Handler trace =
                new Handler() {
                    @Override
                    public void publish(LogRecord record) {
                        String message = record.getMessage();
                        traces.add(message);
                        if (message.contains("phase=replied")
                                || message.contains("phase=completed")) terminals.countDown();
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        logger.setLevel(Level.ALL);
        logger.addHandler(trace);
        try {
            ZLinkDispatchOptionsRegistration options = new ZLinkDispatchOptionsRegistration();
            options.messageFlow(ZLinkMessageFlowLogMode.NORMAL);
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
                                            ProbeRouteSendHandler.class,
                                            String.class,
                                            "Send",
                                            java.util.List.of())),
                            Map.of(
                                    "Request",
                                    new ChannelRouteRequestHandlerRegistration(
                                            ProbeRouteRequestHandler.class,
                                            String.class,
                                            String.class,
                                            "Request",
                                            java.util.List.of())));
                } else {
                    registry.registerClientServer(
                            channel,
                            Map.of(
                                    "Send",
                                    new ChannelSendHandlerRegistration(
                                            ProbeSendHandler.class,
                                            String.class,
                                            "Send",
                                            java.util.List.of())),
                            Map.of(
                                    "Request",
                                    new ChannelRequestHandlerRegistration(
                                            ProbeRequestHandler.class,
                                            String.class,
                                            String.class,
                                            "Request",
                                            java.util.List.of())));
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

            probe.sendEntered.get(2, TimeUnit.SECONDS);
            probe.laterReply.get(2, TimeUnit.SECONDS);
            assertFalse(probe.firstReply.isDone());
            assertEquals(List.of("first", "send", "later"), probe.starts);
            assertEquals(List.of("send", "later"), probe.completions);
            registry.sealClosingAdmission();
            var closing = registry.awaitQuiescence().toCompletableFuture();
            assertFalse(closing.isDone());

            assertEquals(
                    4L,
                    traces.stream().filter(line -> line.contains("phase=admitted")).count(),
                    traces.toString());
            assertEquals(
                    4L,
                    traces.stream().filter(line -> line.contains("phase=dispatched")).count(),
                    traces.toString());
            probe.releaseFirst.complete("first-reply");
            probe.firstReply.get(2, TimeUnit.SECONDS);
            probe.sendEntered.get(2, TimeUnit.SECONDS);
            probe.laterEntered.get(2, TimeUnit.SECONDS);
            probe.laterReply.get(2, TimeUnit.SECONDS);
            closing.get(2, TimeUnit.SECONDS);
            assertEquals(List.of("send", "later", "first"), probe.completions);
            assertTrue(terminals.await(2, TimeUnit.SECONDS), traces.toString());
            var byFlow =
                    traces.stream()
                            .filter(line -> line.contains("phase="))
                            .collect(
                                    java.util.stream.Collectors.groupingBy(
                                            line -> line.replaceAll(".* flow=([^ ]+).*", "$1")));
            assertEquals(4, byFlow.size(), traces.toString());
            for (var records : byFlow.values()) {
                boolean send = records.getFirst().contains("kind=send");
                assertEquals(
                        List.of(
                                "received",
                                "admitted",
                                "dispatched",
                                send ? "completed" : "replied"),
                        records.stream()
                                .map(line -> line.replaceAll(".* phase=([^ ]+).*", "$1"))
                                .toList(),
                        records.toString());
                assertEquals(
                        1L,
                        records.stream()
                                .map(
                                        line ->
                                                line.contains(" corr=")
                                                        ? line.replaceAll(".* corr=([^ ]+).*", "$1")
                                                        : "")
                                .distinct()
                                .count());
            }
            assertEquals(
                    1L,
                    traces.stream().filter(line -> line.contains("phase=completed")).count(),
                    traces.toString());
            assertEquals(
                    4L,
                    traces.stream().filter(line -> line.contains("phase=dispatched")).count(),
                    traces.toString());
        } finally {
            logger.removeHandler(trace);
            logger.setLevel(previousLevel);
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
                                : parts -> {
                                    var failure = ZLinkFrameworkErrorReply.failure(parts);
                                    reply.complete(
                                            failure == null
                                                    ? parts.getLast().toUtf8String()
                                                    : "error:" + failure.kind());
                                },
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
        private final List<Integer> filterStarts = new CopyOnWriteArrayList<>();
        private final CompletableFuture<Void> filterRelease = new CompletableFuture<>();
        private final AtomicInteger running = new AtomicInteger();
        private final AtomicInteger maximumRunning = new AtomicInteger();

        private final List<String> starts = new CopyOnWriteArrayList<>();
        private final List<String> destroyed = new CopyOnWriteArrayList<>();
        private final List<String> completions = new CopyOnWriteArrayList<>();

        private CompletionStage<String> request(String value) {
            if (value.equals("other")) {
                return CompletableFuture.completedFuture("other-reply");
            }
            starts.add(value);
            int active = running.incrementAndGet();
            maximumRunning.accumulateAndGet(active, Math::max);
            if (value.equals("first")) {
                firstEntered.complete(null);
                return releaseFirst.whenComplete(
                        (ignored, error) -> {
                            completions.add("first");
                            running.decrementAndGet();
                        });
            }
            completions.add(value);
            laterEntered.complete(null);
            running.decrementAndGet();
            return CompletableFuture.completedFuture("later-reply");
        }

        private CompletionStage<Void> send(String value) {
            CompletionStage<String> terminal = request(value);
            sendEntered.complete(null);
            return terminal.thenApply(ignored -> null);
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
            return gateProbe.send(message);
        }
    }

    public static final class ProbeRouteSendHandler implements ZLinkRouteSendHandler<String> {
        @Override
        public CompletionStage<Void> handle(String message, ZLinkRouteMessageContext context) {
            return gateProbe.send(message);
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
                                    "Request",
                                    java.util.List.of())));
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
                                    "Request",
                                    java.util.List.of())));
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
        return invoker(ZLinkHandlerActivator.reflection(), List.of());
    }

    private static ZLinkChannelHandlerInvoker invoker(
            ZLinkHandlerActivator activator,
            List<Class<? extends systems.zlink.framework.ZLinkHandlerFilter>> filters) {
        return new ZLinkChannelHandlerInvoker(
                new ZLinkStringMessageSerializer(),
                new ZLinkCodecRegistration(),
                activator,
                Runnable::run,
                filters);
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
