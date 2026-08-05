package systems.zlink.framework.runtime;

import systems.zlink.framework.spots.SpotHandleResolver;
import systems.zlink.framework.spots.SpotHandle;

import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;

import systems.zlink.framework.runtime.internal.backend.*;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.net.ServerSocket;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.time.Duration;
import java.util.List;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;
import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.ZLinkHandlerFilter;
import systems.zlink.framework.ZLinkHandlerDispatchKind;
import systems.zlink.framework.ZLinkHandlerFilterContext;
import systems.zlink.framework.ZLinkHandlerFilterNext;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.configuration.ZLinkDispatchErrorAction;
import systems.zlink.framework.configuration.ZLinkDispatchErrorReason;
import systems.zlink.framework.configuration.ZLinkDispatchErrorSurface;
import systems.zlink.framework.configuration.ZLinkDispatchMessageKind;
import systems.zlink.framework.configuration.ZLinkMessageFlowEvent;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.channels.ZLinkFanoutHandler;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.channels.ZLinkRouteSendHandler;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.handlers.ZLinkPublish;
import systems.zlink.framework.handlers.ZLinkPacket;
import systems.zlink.framework.handlers.ZLinkRequest;
import systems.zlink.framework.handlers.ZLinkSend;
import systems.zlink.framework.handlers.ZLinkSpotRequest;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotKind;

final class ChannelMessagingTest {
    private static final AtomicInteger NEXT_PORT =
        new AtomicInteger(32_000 + (int) (ProcessHandle.current().pid() % 10_000));
    private static final AtomicReference<CountDownLatch> FANOUT_LATCH = new AtomicReference<>();
    private static final AtomicReference<String> FANOUT_MESSAGE = new AtomicReference<>();
    private static final AtomicReference<String> FANOUT_TOPIC = new AtomicReference<>();
    private static final AtomicReference<String> FANOUT_CHANNEL = new AtomicReference<>();
    private static final CopyOnWriteArrayList<String> FANOUT_SEQUENCE_ONE = new CopyOnWriteArrayList<>();
    private static final CopyOnWriteArrayList<String> FANOUT_SEQUENCE_TWO = new CopyOnWriteArrayList<>();
    private static final CopyOnWriteArrayList<String> FANOUT_SEQUENCE_THREE = new CopyOnWriteArrayList<>();
    private static final AtomicReference<CountDownLatch> MANUAL_REG_PUBLISH_LATCH = new AtomicReference<>();
    private static final AtomicReference<String> MANUAL_REG_PUBLISH_MESSAGE = new AtomicReference<>();
    private static final AtomicReference<String> MANUAL_REG_PUBLISH_TOPIC = new AtomicReference<>();
    private static final AtomicReference<String> MANUAL_REG_PUBLISH_CHANNEL = new AtomicReference<>();
    private static final AtomicReference<CountDownLatch> SEND_LATCH = new AtomicReference<>();
    private static final AtomicReference<String> SEND_MESSAGE = new AtomicReference<>();
    private static final AtomicReference<String> SEND_PACKET = new AtomicReference<>();
    private static final AtomicReference<String> SEND_CHANNEL = new AtomicReference<>();
    private static final AtomicReference<CountDownLatch> MANUAL_REG_SEND_LATCH = new AtomicReference<>();
    private static final AtomicReference<String> MANUAL_REG_SEND_MESSAGE = new AtomicReference<>();
    private static final AtomicReference<String> MANUAL_REG_SEND_PACKET = new AtomicReference<>();
    private static final AtomicReference<String> MANUAL_REG_SEND_CHANNEL = new AtomicReference<>();
    private static final AtomicReference<CountDownLatch> ROUTE_SEND_LATCH = new AtomicReference<>();
    private static final AtomicReference<String> ROUTE_SEND_MESSAGE = new AtomicReference<>();
    private static final AtomicReference<String> ROUTE_SEND_PACKET = new AtomicReference<>();
    private static final AtomicReference<String> ROUTE_SEND_CHANNEL = new AtomicReference<>();
    private static final AtomicReference<RoutingId> ROUTE_SEND_SOURCE = new AtomicReference<>();
    private static final AtomicReference<String> ROUTE_REQUEST_CHANNEL = new AtomicReference<>();
    private static final AtomicReference<String> FILTER_PACKET = new AtomicReference<>();
    private static final AtomicReference<String> FILTER_CHANNEL = new AtomicReference<>();
    private static final AtomicReference<String> FILTER_MESH = new AtomicReference<>();
    private static final AtomicReference<ZLinkHandlerDispatchKind> FILTER_KIND =
        new AtomicReference<>();
    private static final RoutingId SPOT_EGRESS_TARGET_NODE_RID =
        RoutingId.from("spot-egress-target-spot");
    private static final RoutingId SPOT_EGRESS_TARGET_ROUTE_RID =
        RoutingId.from("spot-egress-target-route");

    @Test
    @DisplayName("CH-001 manual client-server request-response")
    void manualClientServer_requestReplySucceeds() {
        String endpoint = "inproc://zlink-java-profile-" + UUID.randomUUID();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        { var channel = options.addClientServerChannel("profile").server().listen();
            options.addClientServerChannel("profile").client();
            channel.addRequestHandler(EchoHandler.class, EchoRequest.class, String.class); };

        try (ZLinkFrameworkRuntime runtime =
                 RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {
            String reply = runtime.client()
                .requestToChannel("profile", new EchoRequest("hello"))
                .submit(String.class)
                .toCompletableFuture()
                .join();

            assertEquals("hello", reply);
        }
    }

    @Test
    void processLocalClientServer_requestReplySucceedsWithoutStoreOrManualClientEndpoint() {
        String endpoint = "inproc://zlink-java-local-profile-" + UUID.randomUUID();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addClientServerChannel("profile").client();
        var server = options.addClientServerChannel("profile").server().listen();
        server.addRequestHandler(
            EchoHandler.class, EchoRequest.class, String.class);

        try (ZLinkFrameworkRuntime runtime =
                 RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {
            String reply = runtime.client()
                .requestToChannel("profile", new EchoRequest("hello"))
                .submit(String.class)
                .toCompletableFuture()
                .join();

            assertEquals("hello", reply);
        }
    }

    @Test
    void processLocalClientServer_withZeroWeightIsNotSelected() {
        String endpoint = "inproc://zlink-java-zero-weight-" + UUID.randomUUID();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addClientServerChannel("profile").client();
        var server = options.addClientServerChannel("profile").server().listen();
        server.setWeight(0);
        server.addRequestHandler(
            EchoHandler.class, EchoRequest.class, String.class);

        try (ZLinkFrameworkRuntime runtime =
                 RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {
            ZLinkFrameworkException failure = assertThrows(
                ZLinkFrameworkException.class,
                () -> runtime.client()
                    .requestToChannel("profile", new EchoRequest("hello")));

            assertEquals(
                systems.zlink.framework.errors.ZLinkFrameworkErrorKind
                    .NOT_FOUND,
                failure.kind());
        }
    }

    @Test
    void handlerFiltersWrapChannelRequestDispatch() {
        String endpoint = "inproc://zlink-java-filtered-profile-" + UUID.randomUUID();
        FILTER_PACKET.set(null);
        FILTER_CHANNEL.set(null);
        FILTER_MESH.set(null);
        FILTER_KIND.set(null);

        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.useFilter(ReplyDecoratingFilter.class);
        { var channel = options.addClientServerChannel("profile").server().listen();
            options.addClientServerChannel("profile").client();
            channel.addRequestHandler(EchoHandler.class, EchoRequest.class, String.class); };

        try (ZLinkFrameworkRuntime runtime =
                 RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {
            String reply = runtime.client()
                .requestToChannel("profile", new EchoRequest("hello"))
                .submit(String.class)
                .toCompletableFuture()
                .join();

            assertEquals("hello", reply);
            assertEquals("Echo", FILTER_PACKET.get());
            assertEquals("profile", FILTER_CHANNEL.get());
            assertEquals("", FILTER_MESH.get());
            assertEquals(
                ZLinkHandlerDispatchKind.CHANNEL_REQUEST,
                FILTER_KIND.get());
        } finally {
            FILTER_PACKET.set(null);
            FILTER_CHANNEL.set(null);
            FILTER_MESH.set(null);
            FILTER_KIND.set(null);
        }
    }

    @Test
    @DisplayName("CH-006 manual client-server send one-way records server evidence")
    void manualClientServer_sendDispatchesToHandler() throws InterruptedException {
        String endpoint = "inproc://zlink-java-notify-" + UUID.randomUUID();
        CountDownLatch latch = new CountDownLatch(1);
        SEND_LATCH.set(latch);
        SEND_MESSAGE.set(null);
        SEND_PACKET.set(null);
        SEND_CHANNEL.set(null);

        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        { var channel = options.addClientServerChannel("profile").server().listen();
            options.addClientServerChannel("profile").client();
            channel.addSendHandler(ProfileChangedHandler.class, ProfileChanged.class); };

        try (ZLinkFrameworkRuntime runtime =
                 RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {
            sendUntilDelivered(runtime);

            assertTrue(latch.await(1, TimeUnit.SECONDS), "client/server send was not delivered");
            assertEquals("changed", SEND_MESSAGE.get());
            assertEquals("ProfileChanged", SEND_PACKET.get());
            assertEquals("profile", SEND_CHANNEL.get());
        } finally {
            SEND_LATCH.set(null);
            SEND_MESSAGE.set(null);
            SEND_PACKET.set(null);
            SEND_CHANNEL.set(null);
        }
    }

    @Test
    @DisplayName("DERR-001 manual client-server missing request handler replies error and reports observer")
    void manualClientServer_missingRequestHandlerRepliesErrorAndReportsObserver()
        throws InterruptedException {
        String endpoint = "inproc://zlink-java-dispatch-error-" + UUID.randomUUID();
        CountDownLatch errorLatch = new CountDownLatch(1);
        AtomicReference<ZLinkMessageFlowEvent> observedError = new AtomicReference<>();
        CopyOnWriteArrayList<String> logMessages = new CopyOnWriteArrayList<>();
        Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        Handler logHandler = new Handler() {
            @Override
            public void publish(LogRecord record) {
                logMessages.add(record.getMessage());
            }

            @Override
            public void flush() {
            }

            @Override
            public void close() {
            }
        };
        logger.addHandler(logHandler);

        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        { var dispatch = options.configureDispatch();
            dispatch.setMessageFlowObserver(error -> {
                observedError.set(error);
                errorLatch.countDown();
                return CompletableFuture.completedFuture(null);
            }); };
        { var channel = options.addClientServerChannel("profile").server().listen();
            options.addClientServerChannel("profile").client();
            channel.addRequestHandler(EchoHandler.class, EchoRequest.class, String.class); };

        try (ZLinkFrameworkRuntime runtime =
                 RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {
            String before = runtime.client()
                .requestToChannel("profile", new EchoRequest("before"))
                .submit(String.class)
                .toCompletableFuture()
                .join();
            assertEquals("before", before);
            String markerText = runtime.client()
                .requestToChannel("profile", new EchoRequest("ZLinkFrameworkError:not-error"))
                .submit(String.class)
                .toCompletableFuture()
                .join();
            assertEquals("ZLinkFrameworkError:not-error", markerText);
            String markerOnlyText = runtime.client()
                .requestToChannel("profile", new EchoRequest("ZLinkFrameworkError"))
                .submit(String.class)
                .toCompletableFuture()
                .join();
            assertEquals("ZLinkFrameworkError", markerOnlyText);

            CompletionException failure = assertThrows(CompletionException.class, () -> runtime.client()
                .requestToChannel("profile", new MissingRequest("missing"))
                .submit(String.class)
                .toCompletableFuture()
                .join());
            assertTrue(failure.getCause() instanceof ZLinkFrameworkException);
            assertTrue(failure.getCause().getMessage().contains(
                "HANDLER_MISSING for packet 'MissingReq'"));

            assertTrue(errorLatch.await(1, TimeUnit.SECONDS),
                "dispatch error observer was not called");
            ZLinkMessageFlowEvent error = observedError.get();
            assertEquals(ZLinkDispatchErrorSurface.CHANNEL, error.surface());
            assertEquals(ZLinkDispatchMessageKind.REQUEST, error.messageKind());
            assertEquals(ZLinkDispatchErrorReason.HANDLER_MISSING, error.errorReason());
            assertEquals(ZLinkDispatchErrorAction.REPLY_ERROR, error.errorAction());
            assertEquals("MissingReq", error.packetName());
            assertEquals("profile", error.channelName());
            assertTrue(logMessages.stream().anyMatch(message ->
                message.contains("reason=HANDLER_MISSING")
                    && message.contains("action=REPLY_ERROR")
                    && message.contains("packet=MissingReq")
                    && message.contains("channel=profile")),
                "dispatch error log marker was not written");

            String after = runtime.client()
                .requestToChannel("profile", new EchoRequest("after"))
                .submit(String.class)
                .toCompletableFuture()
                .join();
            assertEquals("after", after);
        } finally {
            logger.removeHandler(logHandler);
        }
    }

    @Test
    @DisplayName("DERR-002 manual client-server missing send handler reports observer and keeps request path alive")
    void manualClientServer_missingSendHandlerReportsObserverAndKeepsRequestPathAlive()
        throws InterruptedException {
        String endpoint = "inproc://zlink-java-dispatch-send-error-" + UUID.randomUUID();
        CountDownLatch errorLatch = new CountDownLatch(1);
        AtomicReference<ZLinkMessageFlowEvent> observedError = new AtomicReference<>();
        CopyOnWriteArrayList<String> logMessages = new CopyOnWriteArrayList<>();
        Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        Handler logHandler = new Handler() {
            @Override
            public void publish(LogRecord record) {
                logMessages.add(record.getMessage());
            }

            @Override
            public void flush() {
            }

            @Override
            public void close() {
            }
        };
        logger.addHandler(logHandler);

        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        { var dispatch = options.configureDispatch();
            dispatch.setMessageFlowObserver(error -> {
                observedError.set(error);
                errorLatch.countDown();
                return CompletableFuture.completedFuture(null);
            }); };
        { var channel = options.addClientServerChannel("profile").server().listen();
            options.addClientServerChannel("profile").client();
            channel.addRequestHandler(EchoHandler.class, EchoRequest.class, String.class); };

        try (ZLinkFrameworkRuntime runtime =
                 RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {
            String before = runtime.client()
                .requestToChannel("profile", new EchoRequest("before-send-error"))
                .submit(String.class)
                .toCompletableFuture()
                .join();
            assertEquals("before-send-error", before);

            runtime.client()
                .sendToChannel("profile", new MissingCommand("missing-send"))
                .submit();

            assertTrue(errorLatch.await(1, TimeUnit.SECONDS),
                "dispatch error observer was not called");
            ZLinkMessageFlowEvent error = observedError.get();
            assertEquals(ZLinkDispatchErrorSurface.CHANNEL, error.surface());
            assertEquals(ZLinkDispatchMessageKind.SEND, error.messageKind());
            assertEquals(ZLinkDispatchErrorReason.HANDLER_MISSING, error.errorReason());
            assertEquals(ZLinkDispatchErrorAction.DROP, error.errorAction());
            assertEquals("MissingCommand", error.packetName());
            assertEquals("profile", error.channelName());
            assertTrue(logMessages.stream().anyMatch(message ->
                message.contains("reason=HANDLER_MISSING")
                    && message.contains("action=DROP")
                    && message.contains("packet=MissingCommand")
                    && message.contains("channel=profile")),
                "dispatch error log marker was not written");

            String after = runtime.client()
                .requestToChannel("profile", new EchoRequest("after-send-error"))
                .submit(String.class)
                .toCompletableFuture()
                .join();
            assertEquals("after-send-error", after);
        } finally {
            logger.removeHandler(logHandler);
        }
    }

    @Test
    @DisplayName("DERR-006 manual client-server payload decode failure replies error and reports observer")
    void manualClientServer_payloadDecodeFailureRepliesErrorAndReportsObserver()
        throws Exception {
        String endpoint = tcpEndpoint();
        CountDownLatch errorLatch = new CountDownLatch(1);
        AtomicReference<ZLinkMessageFlowEvent> observedError = new AtomicReference<>();
        CopyOnWriteArrayList<String> logMessages = new CopyOnWriteArrayList<>();
        Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        Handler logHandler = new Handler() {
            @Override
            public void publish(LogRecord record) {
                logMessages.add(record.getMessage());
            }

            @Override
            public void flush() {
            }

            @Override
            public void close() {
            }
        };
        logger.addHandler(logHandler);
        DecodeProbeHandler.invocations.set(0);

        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        { var dispatch = options.configureDispatch();
            dispatch.setMessageFlowObserver(error -> {
                observedError.set(error);
                errorLatch.countDown();
                return CompletableFuture.completedFuture(null);
            }); };
        { var channel = listenClientServer(options, "profile", endpoint);
            options.addClientServerChannel("profile").client().connect(endpoint);
            channel.addRequestHandler(EchoHandler.class, EchoRequest.class, String.class);
            channel.addRequestHandler(DecodeProbeHandler.class, DecodePayload.class, String.class); };

        ZLinkJavaBackendAdapterFactory backendFactory = new ZLinkJavaBackendAdapterFactory();
        try (ZLinkFrameworkRuntime runtime = RuntimeTestSupport.startFramework(options, backendFactory)) {
            String before = runtime.client()
                .requestToChannel("profile", new EchoRequest("before-decode-error"))
                .submit(String.class)
                .toCompletableFuture()
                .join();
            assertEquals("before-decode-error", before);

            var channelAdapter = backendFactory.createChannelAdapter(
                new ZLinkBackendAdapterOptions(Duration.ofSeconds(1)));
            try (var rawContext = channelAdapter.createContext();
                var rawDealer = channelAdapter.createDealerSocket(rawContext)) {
                rawDealer.connect(endpoint);
                Thread.sleep(100);
                CompletableFuture<ZLinkBackendReceived> replyFuture = new CompletableFuture<>();
                List<Message> malformedParts = List.of(
                    Message.from("DecodeReq"),
                    Message.from("{"));
                try {
                    assertTrue(rawDealer.request(
                        malformedParts,
                        replyFuture::complete,
                        SendFlags.NONE,
                        Duration.ofSeconds(2)));
                    try (ZLinkBackendReceived reply = replyFuture.get(2, TimeUnit.SECONDS)) {
                        assertEquals("ZLinkFrameworkError", reply.parts().get(0).toUtf8String());
                        assertTrue(reply.parts().get(1).toUtf8String().contains("PayloadDecodeFailed"));
                    }
                } finally {
                    malformedParts.forEach(Message::close);
                }
            }

            assertEquals(0, DecodeProbeHandler.invocations.get());
            assertTrue(errorLatch.await(1, TimeUnit.SECONDS),
                "dispatch error observer was not called");
            ZLinkMessageFlowEvent error = observedError.get();
            assertEquals(ZLinkDispatchErrorSurface.CHANNEL, error.surface());
            assertEquals(ZLinkDispatchMessageKind.REQUEST, error.messageKind());
            assertEquals(ZLinkDispatchErrorReason.PAYLOAD_DECODE_FAILED, error.errorReason());
            assertEquals(ZLinkDispatchErrorAction.REPLY_ERROR, error.errorAction());
            assertEquals("DecodeReq", error.packetName());
            assertEquals("profile", error.channelName());
            assertEquals("PayloadDecodeDispatchException", error.errorType());
            assertTrue(error.errorMessage().contains("PayloadDecodeFailed"));
            assertTrue(logMessages.stream().anyMatch(message ->
                message.contains("reason=PAYLOAD_DECODE_FAILED")
                    && message.contains("action=REPLY_ERROR")
                    && message.contains("packet=DecodeReq")
                    && message.contains("channel=profile")),
                "dispatch error log marker was not written");

            String after = runtime.client()
                .requestToChannel("profile", new DecodePayload("after-decode-error"))
                .submit(String.class)
                .toCompletableFuture()
                .join();
            assertEquals("decode:after-decode-error", after);
            assertEquals(1, DecodeProbeHandler.invocations.get());
        } finally {
            logger.removeHandler(logHandler);
            DecodeProbeHandler.invocations.set(0);
        }
    }

    @Test
    @DisplayName("DERR-007 manual client-server handler exception replies error and reports observer")
    void manualClientServer_handlerExceptionRepliesErrorAndReportsObserver()
        throws InterruptedException {
        String endpoint = tcpEndpoint();
        CountDownLatch errorLatch = new CountDownLatch(1);
        AtomicReference<ZLinkMessageFlowEvent> observedError = new AtomicReference<>();
        CopyOnWriteArrayList<String> logMessages = new CopyOnWriteArrayList<>();
        Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        Handler logHandler = new Handler() {
            @Override
            public void publish(LogRecord record) {
                logMessages.add(record.getMessage());
            }

            @Override
            public void flush() {
            }

            @Override
            public void close() {
            }
        };
        logger.addHandler(logHandler);

        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        { var dispatch = options.configureDispatch();
            dispatch.setMessageFlowObserver(error -> {
                observedError.set(error);
                errorLatch.countDown();
                return CompletableFuture.completedFuture(null);
            }); };
        { var channel = listenClientServer(options, "profile", endpoint);
            options.addClientServerChannel("profile").client().connect(endpoint);
            channel.addRequestHandler(EchoHandler.class, EchoRequest.class, String.class);
            channel.addRequestHandler(ThrowingRequestHandler.class, ThrowRequest.class, String.class); };

        try (ZLinkFrameworkRuntime runtime =
                 RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {
            String before = runtime.client()
                .requestToChannel("profile", new EchoRequest("before-handler-error"))
                .submit(String.class)
                .toCompletableFuture()
                .join();
            assertEquals("before-handler-error", before);

            CompletionException failure = assertThrows(CompletionException.class, () -> runtime.client()
                .requestToChannel("profile", new ThrowRequest("boom"))
                .submit(String.class)
                .toCompletableFuture()
                .join());
            assertTrue(failure.getCause() instanceof ZLinkFrameworkException);
            assertTrue(failure.getCause().getMessage().contains("DERR-007 handler exception"));

            assertTrue(errorLatch.await(1, TimeUnit.SECONDS),
                "dispatch error observer was not called");
            ZLinkMessageFlowEvent error = observedError.get();
            assertEquals(ZLinkDispatchErrorSurface.CHANNEL, error.surface());
            assertEquals(ZLinkDispatchMessageKind.REQUEST, error.messageKind());
            assertEquals(ZLinkDispatchErrorReason.HANDLER_EXCEPTION, error.errorReason());
            assertEquals(ZLinkDispatchErrorAction.REPLY_ERROR, error.errorAction());
            assertEquals("ThrowReq", error.packetName());
            assertEquals("profile", error.channelName());
            assertEquals("IllegalStateException", error.errorType());
            assertEquals("DERR-007 handler exception", error.errorMessage());
            assertTrue(logMessages.stream().anyMatch(message ->
                message.contains("reason=HANDLER_EXCEPTION")
                    && message.contains("action=REPLY_ERROR")
                    && message.contains("packet=ThrowReq")
                    && message.contains("channel=profile")),
                "dispatch error log marker was not written");

            String after = runtime.client()
                .requestToChannel("profile", new EchoRequest("after-handler-error"))
                .submit(String.class)
                .toCompletableFuture()
                .join();
            assertEquals("after-handler-error", after);
        } finally {
            logger.removeHandler(logHandler);
        }
    }

    @Test
    @DisplayName("DERR-009 manual client-server dispatch errors are written to file log")
    void manualClientServer_dispatchErrorsAreWrittenToFileLog() throws Exception {
        String endpoint = tcpEndpoint();
        Path logPath = Files.createTempFile("zlink-java-derr-009-", ".log");
        CountDownLatch errors = new CountDownLatch(3);

        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        { var dispatch = options.configureDispatch();
            dispatch.setMessageFlowObserver(error -> {
                try {
                    Files.writeString(
                        logPath,
                        dispatchLogLine(error),
                        StandardCharsets.UTF_8,
                        StandardOpenOption.CREATE,
                        StandardOpenOption.APPEND);
                } catch (IOException ex) {
                    return CompletableFuture.failedFuture(ex);
                }
                errors.countDown();
                return CompletableFuture.completedFuture(null);
            }); };
        { var channel = listenClientServer(options, "profile", endpoint);
            options.addClientServerChannel("profile").client().connect(endpoint);
            channel.addRequestHandler(EchoHandler.class, EchoRequest.class, String.class);
            channel.addRequestHandler(DecodeProbeHandler.class, DecodePayload.class, String.class);
            channel.addRequestHandler(ThrowingRequestHandler.class, ThrowRequest.class, String.class); };

        ZLinkJavaBackendAdapterFactory backendFactory = new ZLinkJavaBackendAdapterFactory();
        DecodeProbeHandler.invocations.set(0);
        try (ZLinkFrameworkRuntime runtime = RuntimeTestSupport.startFramework(options, backendFactory)) {
            String before = runtime.client()
                .requestToChannel("profile", new EchoRequest("before-file-log"))
                .submit(String.class)
                .toCompletableFuture()
                .join();
            assertEquals("before-file-log", before);

            CompletionException missing = assertThrows(CompletionException.class, () -> runtime.client()
                .requestToChannel("profile", new MissingRequest("missing"))
                .submit(String.class)
                .toCompletableFuture()
                .join());
            assertTrue(missing.getCause() instanceof ZLinkFrameworkException);

            var channelAdapter = backendFactory.createChannelAdapter(
                new ZLinkBackendAdapterOptions(Duration.ofSeconds(1)));
            try (var rawContext = channelAdapter.createContext();
                 var rawDealer = channelAdapter.createDealerSocket(rawContext)) {
                rawDealer.connect(endpoint);
                Thread.sleep(100);
                CompletableFuture<ZLinkBackendReceived> replyFuture = new CompletableFuture<>();
                List<Message> malformedParts = List.of(
                    Message.from("DecodeReq"),
                    Message.from("{"));
                try {
                    assertTrue(rawDealer.request(
                        malformedParts,
                        replyFuture::complete,
                        SendFlags.NONE,
                        Duration.ofSeconds(2)));
                    try (ZLinkBackendReceived reply = replyFuture.get(2, TimeUnit.SECONDS)) {
                        assertEquals("ZLinkFrameworkError", reply.parts().get(0).toUtf8String());
                        assertTrue(reply.parts().get(1).toUtf8String().contains("PayloadDecodeFailed"));
                    }
                } finally {
                    malformedParts.forEach(Message::close);
                }
            }

            CompletionException thrown = assertThrows(CompletionException.class, () -> runtime.client()
                .requestToChannel("profile", new ThrowRequest("boom"))
                .submit(String.class)
                .toCompletableFuture()
                .join());
            assertTrue(thrown.getCause() instanceof ZLinkFrameworkException);

            assertTrue(errors.await(2, TimeUnit.SECONDS), "dispatch error file log was not written");
            String logText = waitForFileLog(logPath, "dispatch-error", 3);
            assertTrue(logText.contains("surface=CHANNEL"));
            assertTrue(logText.contains("messageKind=REQUEST"));
            assertTrue(logText.contains("reason=HANDLER_MISSING"));
            assertTrue(logText.contains("reason=PAYLOAD_DECODE_FAILED"));
            assertTrue(logText.contains("reason=HANDLER_EXCEPTION"));
            assertTrue(logText.contains("action=REPLY_ERROR"));
            assertTrue(logText.contains("packetName=MissingReq"));
            assertTrue(logText.contains("packetName=DecodeReq"));
            assertTrue(logText.contains("packetName=ThrowReq"));
            assertTrue(logText.contains("channelName=profile"));
            assertTrue(logText.contains("correlationId="));
        } finally {
            DecodeProbeHandler.invocations.set(0);
            Files.deleteIfExists(logPath);
        }
    }

    @Test
    @DisplayName("CDC-001 JSON codec round-trips nested arrays and nullable fields")
    void jsonCodec_roundTripsNestedArraysAndNullableFields() {
        String endpoint = "inproc://zlink-java-codec-" + UUID.randomUUID();
        JsonCodecProbe request = new JsonCodecProbe(
            "root",
            42,
            null,
            List.of("alpha", "beta"),
            List.of(
                new JsonCodecChild("first", 1),
                new JsonCodecChild("second", 2)),
            new JsonCodecChild("nested", 3));

        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        { var channel = options.addClientServerChannel("codec").server().listen();
            options.addClientServerChannel("codec").client();
            channel.addRequestHandler(
                JsonCodecEchoHandler.class,
                JsonCodecProbe.class,
                JsonCodecProbe.class); };

        try (ZLinkFrameworkRuntime runtime =
                 RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {
            JsonCodecProbe reply = runtime.client()
                .requestToChannel("codec", request)
                .submit(JsonCodecProbe.class)
                .toCompletableFuture()
                .join();

            assertEquals(request, reply);
            assertNull(reply.optionalLabel());
            assertEquals(List.of("alpha", "beta"), reply.tags());
            assertEquals(new JsonCodecChild("nested", 3), reply.nested());
        }
    }

    @Test
    @DisplayName("REG-003 manual channel handlers dispatch registered packets and report missing packets")
    void manualChannelHandlers_dispatchRegisteredPacketsAndReportMissingPackets()
        throws Exception {
        String endpoint = tcpEndpoint();
        String fanoutEndpoint = tcpEndpoint();
        CountDownLatch sendLatch = new CountDownLatch(1);
        CountDownLatch publishLatch = new CountDownLatch(1);
        CountDownLatch errorLatch = new CountDownLatch(3);
        CopyOnWriteArrayList<ZLinkMessageFlowEvent> observedErrors = new CopyOnWriteArrayList<>();
        MANUAL_REG_SEND_LATCH.set(sendLatch);
        MANUAL_REG_SEND_MESSAGE.set(null);
        MANUAL_REG_SEND_PACKET.set(null);
        MANUAL_REG_SEND_CHANNEL.set(null);
        MANUAL_REG_PUBLISH_LATCH.set(publishLatch);
        MANUAL_REG_PUBLISH_MESSAGE.set(null);
        MANUAL_REG_PUBLISH_TOPIC.set(null);
        MANUAL_REG_PUBLISH_CHANNEL.set(null);

        DefaultZLinkFrameworkOptions serverOptions = new DefaultZLinkFrameworkOptions();
        { var dispatch = serverOptions.configureDispatch();
            dispatch.setMessageFlowObserver(error -> {
                observedErrors.add(error);
                errorLatch.countDown();
                return CompletableFuture.completedFuture(null);
            }); };
        { var channel = listenClientServer(serverOptions, "manual-reg", endpoint);
            channel.addRequestHandler(
                ManualRegistrationRequestHandler.class,
                ManualRequest.class,
                String.class);
            channel.addSendHandler(
                ManualRegistrationCommandHandler.class,
                ManualCommand.class); };
        serverOptions.addClientServerChannel("manual-reg").client();

        DefaultZLinkFrameworkOptions publisherOptions = new DefaultZLinkFrameworkOptions();
        { var channel = publisherOptions.addFanoutChannel("manual-events").enablePublisher(fanoutEndpoint); };

        DefaultZLinkFrameworkOptions subscriberOptions = new DefaultZLinkFrameworkOptions();
        { var dispatch = subscriberOptions.configureDispatch();
            dispatch.setMessageFlowObserver(error -> {
                observedErrors.add(error);
                errorLatch.countDown();
                return CompletableFuture.completedFuture(null);
            }); };
        { var channel = subscriberOptions.addFanoutChannel("manual-events");
            channel.connect(fanoutEndpoint);
            channel.addPublishHandler(
                ManualRegistrationPublishHandler.class,
                ManualEvent.class,
                "ManualRegisteredEvent"); };

        try (ZLinkFrameworkRuntime client =
                 RuntimeTestSupport.startFramework(serverOptions, new ZLinkJavaBackendAdapterFactory());
             ZLinkFrameworkRuntime publisher =
                 RuntimeTestSupport.startFramework(publisherOptions, new ZLinkJavaBackendAdapterFactory());
             ZLinkFrameworkRuntime ignoredSubscriber =
                 RuntimeTestSupport.startFramework(subscriberOptions, new ZLinkJavaBackendAdapterFactory())) {
            String reply = awaitChannelReply(
                client,
                "manual-reg",
                new ManualRequest("registered"),
                String.class);
            assertEquals("manual:registered", reply);

            client.client()
                .sendToChannel("manual-reg", new ManualCommand("command"))
                .submit();
            assertTrue(sendLatch.await(1, TimeUnit.SECONDS), "manual send was not delivered");
            assertEquals("command", MANUAL_REG_SEND_MESSAGE.get());
            assertEquals("ManualRegisteredCommand", MANUAL_REG_SEND_PACKET.get());
            assertEquals("manual-reg", MANUAL_REG_SEND_CHANNEL.get());

            publishManualRegistrationUntilDelivered(publisher, "ManualRegisteredEvent", "published");
            assertTrue(publishLatch.await(1, TimeUnit.SECONDS), "manual publish was not delivered");
            assertEquals("published", MANUAL_REG_PUBLISH_MESSAGE.get());
            assertEquals("manual", MANUAL_REG_PUBLISH_TOPIC.get());
            assertEquals("manual-events", MANUAL_REG_PUBLISH_CHANNEL.get());

            CompletionException missingRequest = assertThrows(CompletionException.class, () -> client.client()
                .requestToChannel("manual-reg", new ManualMissingRequest("missing"))
                .submit(String.class)
                .toCompletableFuture()
                .join());
            assertTrue(missingRequest.getCause() instanceof ZLinkFrameworkException);
            assertTrue(missingRequest.getCause().getMessage().contains(
                "HANDLER_MISSING for packet 'ManualMissingReq'"));

            client.client()
                .sendToChannel("manual-reg", new ManualMissingCommand("missing-command"))
                .submit();

            publishManualRegistrationUntilObserved(
                publisher,
                observedErrors,
                "ManualMissingEvent");

            assertTrue(errorLatch.await(1, TimeUnit.SECONDS), "manual registration errors were not reported");
            assertTrue(waitForManualRegistrationErrors(observedErrors),
                "manual registration request, send, and publish errors were not all reported");
            assertTrue(hasDispatchError(
                observedErrors,
                ZLinkDispatchMessageKind.REQUEST,
                ZLinkDispatchErrorReason.HANDLER_MISSING,
                ZLinkDispatchErrorAction.REPLY_ERROR,
                "ManualMissingReq",
                "manual-reg"));
            assertTrue(hasDispatchError(
                observedErrors,
                ZLinkDispatchMessageKind.SEND,
                ZLinkDispatchErrorReason.HANDLER_MISSING,
                ZLinkDispatchErrorAction.DROP,
                "ManualMissingCommand",
                "manual-reg"));
            assertTrue(hasDispatchError(
                observedErrors,
                ZLinkDispatchMessageKind.PUBLISH,
                ZLinkDispatchErrorReason.HANDLER_MISSING,
                ZLinkDispatchErrorAction.DROP,
                "ManualMissingEvent",
                "manual-events"));
        } finally {
            MANUAL_REG_SEND_LATCH.set(null);
            MANUAL_REG_SEND_MESSAGE.set(null);
            MANUAL_REG_SEND_PACKET.set(null);
            MANUAL_REG_SEND_CHANNEL.set(null);
            MANUAL_REG_PUBLISH_LATCH.set(null);
            MANUAL_REG_PUBLISH_MESSAGE.set(null);
            MANUAL_REG_PUBLISH_TOPIC.set(null);
            MANUAL_REG_PUBLISH_CHANNEL.set(null);
        }
    }

    @Test
    @DisplayName("REG-001 partial scanned handler group request-reply succeeds")
    void scannedHandlerGroup_requestReplySucceeds() {
        String endpoint = "inproc://zlink-java-scanned-profile-" + UUID.randomUUID();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addHandlersFromPackageOf(ChannelMessagingTest.class);
        { var channel = options.addClientServerChannel("profile").server().listen();
            options.addClientServerChannel("profile").client();
            channel.addHandlerGroup("scanned-profile"); };

        try (ZLinkFrameworkRuntime runtime =
                 RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {
            String reply = runtime.client()
                .requestToChannel("profile", new StringPacket("hello"))
                .submit(String.class)
                .toCompletableFuture()
                .join();

            assertEquals("scanned:hello", reply);
        }
    }

    @Test
    @DisplayName("REG-002 partial annotation handler group dispatches request and send")
    void scannedMethodHandlerGroup_requestAndSendDispatch() throws InterruptedException {
        String endpoint = "inproc://zlink-java-annotated-profile-" + UUID.randomUUID();
        CountDownLatch latch = new CountDownLatch(1);
        SEND_LATCH.set(latch);
        SEND_MESSAGE.set(null);
        SEND_PACKET.set(null);

        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addHandlersFromPackageOf(ChannelMessagingTest.class);
        { var channel = options.addClientServerChannel("profile").server().listen();
            options.addClientServerChannel("profile").client();
            channel.addHandlerGroup("annotated-profile"); };

        try (ZLinkFrameworkRuntime runtime =
                 RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {
            String reply = runtime.client()
                .requestToChannel("profile", new AnnotatedEcho("hello"))
                .submit(String.class)
                .toCompletableFuture()
                .join();
            runtime.client()
                .sendToChannel("profile", new ProfileChanged("changed"))
                .submit();

            assertEquals("annotated:hello", reply);
            assertTrue(latch.await(1, TimeUnit.SECONDS), "annotated send was not delivered");
            assertEquals("changed", SEND_MESSAGE.get());
            assertEquals("ProfileChanged", SEND_PACKET.get());
        } finally {
            SEND_LATCH.set(null);
            SEND_MESSAGE.set(null);
            SEND_PACKET.set(null);
        }
    }

    @Test
    @DisplayName("PUB-001 partial REG-002 annotation fanout publish dispatches")
    void scannedMethodHandlerGroup_publishDispatches() throws InterruptedException {
        String endpoint = tcpEndpoint();
        CountDownLatch latch = new CountDownLatch(1);
        FANOUT_LATCH.set(latch);
        FANOUT_MESSAGE.set(null);
        FANOUT_TOPIC.set(null);
        FANOUT_CHANNEL.set(null);

        DefaultZLinkFrameworkOptions publisherOptions = new DefaultZLinkFrameworkOptions();
        { var channel = publisherOptions.addFanoutChannel("events").enablePublisher(endpoint); };

        DefaultZLinkFrameworkOptions subscriberOptions = new DefaultZLinkFrameworkOptions();
        subscriberOptions.addHandlersFromPackageOf(ChannelMessagingTest.class);
        { var channel = subscriberOptions.addFanoutChannel("events"); channel.connect(endpoint);
            channel.addHandlerGroup("annotated-events"); };

        try (ZLinkFrameworkRuntime ignoredPublisher =
                 RuntimeTestSupport.startFramework(publisherOptions, new ZLinkJavaBackendAdapterFactory());
             ZLinkFrameworkRuntime subscriber =
                 RuntimeTestSupport.startFramework(subscriberOptions, new ZLinkJavaBackendAdapterFactory())) {
            publishUntilDelivered(ignoredPublisher);

            assertTrue(latch.await(1, TimeUnit.SECONDS), "annotated publish was not delivered");
            assertEquals("home:1", FANOUT_MESSAGE.get());
            assertEquals("score", FANOUT_TOPIC.get());
        } finally {
            FANOUT_LATCH.set(null);
            FANOUT_MESSAGE.set(null);
            FANOUT_TOPIC.set(null);
        }
    }

    @Test
    @DisplayName("PUB-001 fanout delivers the same sequence to three subscribers")
    void fanout_deliversSameSequenceToThreeSubscribers() {
        String endpoint = tcpEndpoint();
        FANOUT_SEQUENCE_ONE.clear();
        FANOUT_SEQUENCE_TWO.clear();
        FANOUT_SEQUENCE_THREE.clear();

        DefaultZLinkFrameworkOptions publisherOptions = new DefaultZLinkFrameworkOptions();
        { var channel = publisherOptions.addFanoutChannel("sequence").enablePublisher(endpoint); };

        DefaultZLinkFrameworkOptions firstOptions = new DefaultZLinkFrameworkOptions();
        { var channel = firstOptions.addFanoutChannel("sequence");
            channel.connect(endpoint);
            channel.addPublishHandler(FanoutSequenceOneHandler.class, FanoutSequence.class, "FanoutSequence"); };

        DefaultZLinkFrameworkOptions secondOptions = new DefaultZLinkFrameworkOptions();
        { var channel = secondOptions.addFanoutChannel("sequence");
            channel.connect(endpoint);
            channel.addPublishHandler(FanoutSequenceTwoHandler.class, FanoutSequence.class, "FanoutSequence"); };

        DefaultZLinkFrameworkOptions thirdOptions = new DefaultZLinkFrameworkOptions();
        { var channel = thirdOptions.addFanoutChannel("sequence");
            channel.connect(endpoint);
            channel.addPublishHandler(FanoutSequenceThreeHandler.class, FanoutSequence.class, "FanoutSequence"); };

        try (ZLinkFrameworkRuntime publisher =
                 RuntimeTestSupport.startFramework(publisherOptions, new ZLinkJavaBackendAdapterFactory());
             ZLinkFrameworkRuntime ignoredFirst =
                 RuntimeTestSupport.startFramework(firstOptions, new ZLinkJavaBackendAdapterFactory());
             ZLinkFrameworkRuntime ignoredSecond =
                 RuntimeTestSupport.startFramework(secondOptions, new ZLinkJavaBackendAdapterFactory());
             ZLinkFrameworkRuntime ignoredThird =
                 RuntimeTestSupport.startFramework(thirdOptions, new ZLinkJavaBackendAdapterFactory())) {
            String commonSequence = publishUntilCommonFanoutSequence(publisher);

            assertTrue(FANOUT_SEQUENCE_ONE.contains(commonSequence));
            assertTrue(FANOUT_SEQUENCE_TWO.contains(commonSequence));
            assertTrue(FANOUT_SEQUENCE_THREE.contains(commonSequence));
        } finally {
            FANOUT_SEQUENCE_ONE.clear();
            FANOUT_SEQUENCE_TWO.clear();
            FANOUT_SEQUENCE_THREE.clear();
        }
    }

    @Test
    @DisplayName("PUB-001 partial manual fanout publish dispatches across hosts")
    void publisherAndSubscriber_workAcrossHosts() throws InterruptedException {
        String endpoint = tcpEndpoint();
        CountDownLatch latch = new CountDownLatch(1);
        FANOUT_LATCH.set(latch);
        FANOUT_MESSAGE.set(null);
        FANOUT_TOPIC.set(null);

        DefaultZLinkFrameworkOptions publisherOptions = new DefaultZLinkFrameworkOptions();
        { var channel = publisherOptions.addFanoutChannel("events").enablePublisher(endpoint); };

        DefaultZLinkFrameworkOptions subscriberOptions = new DefaultZLinkFrameworkOptions();
        { var channel = subscriberOptions.addFanoutChannel("events"); channel.connect(endpoint);
            channel.addPublishHandler(ScoreChangedHandler.class, ScoreChanged.class, "ScoreChanged"); };

        try (ZLinkFrameworkRuntime ignoredPublisher =
                 RuntimeTestSupport.startFramework(publisherOptions, new ZLinkJavaBackendAdapterFactory());
             ZLinkFrameworkRuntime subscriber =
                 RuntimeTestSupport.startFramework(subscriberOptions, new ZLinkJavaBackendAdapterFactory())) {
            publishUntilDelivered(ignoredPublisher);

            assertTrue(latch.await(1, TimeUnit.SECONDS), "fanout publish was not delivered");
            assertEquals("home:1", FANOUT_MESSAGE.get());
            assertEquals("score", FANOUT_TOPIC.get());
            assertEquals("events", FANOUT_CHANNEL.get());
        } finally {
            FANOUT_LATCH.set(null);
            FANOUT_MESSAGE.set(null);
            FANOUT_TOPIC.set(null);
            FANOUT_CHANNEL.set(null);
        }
    }

    @Test
    void routeMesh_requestByRoutingIdSucceeds() {
        String sourceEndpoint = tcpEndpoint();
        String targetEndpoint = tcpEndpoint();
        RoutingId sourceRid = RoutingId.from("source-node");
        RoutingId targetRid = RoutingId.from("target-node");
        ROUTE_REQUEST_CHANNEL.set(null);

        DefaultZLinkFrameworkOptions sourceOptions = new DefaultZLinkFrameworkOptions();
        { var channel = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(sourceOptions, "route"); channel.enableServer(sourceEndpoint);
            channel.setRoutingId(sourceRid);
            channel.enableClient(targetEndpoint); };

        DefaultZLinkFrameworkOptions targetOptions = new DefaultZLinkFrameworkOptions();
        { var channel = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(targetOptions, "route"); channel.enableServer(targetEndpoint);
            channel.setRoutingId(targetRid);
            channel.enableClient(sourceEndpoint);
            channel.addRequestHandler(RouteEchoHandler.class, EchoRequest.class, String.class, "Echo"); };

        try (ZLinkFrameworkRuntime ignoredSource =
                 RuntimeTestSupport.startFramework(sourceOptions, new ZLinkJavaBackendAdapterFactory());
             ZLinkFrameworkRuntime target =
                 RuntimeTestSupport.startFramework(targetOptions, new ZLinkJavaBackendAdapterFactory())) {
            assertEquals("route:hello", awaitRouteReply(ignoredSource, targetRid));
            assertEquals("route", ROUTE_REQUEST_CHANNEL.get());
        } finally {
            ROUTE_REQUEST_CHANNEL.set(null);
        }
    }

    @Test
    void routeMesh_missingRequestHandlerRepliesFrameworkError() {
        String sourceEndpoint = tcpEndpoint();
        String targetEndpoint = tcpEndpoint();
        RoutingId sourceRid = RoutingId.from("route-missing-source");
        RoutingId targetRid = RoutingId.from("route-missing-target");

        DefaultZLinkFrameworkOptions sourceOptions = new DefaultZLinkFrameworkOptions();
        { var channel = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(sourceOptions, "route"); channel.enableServer(sourceEndpoint);
            channel.setRoutingId(sourceRid);
            channel.enableClient(targetEndpoint); };

        DefaultZLinkFrameworkOptions targetOptions = new DefaultZLinkFrameworkOptions();
        { var channel = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(targetOptions, "route"); channel.enableServer(targetEndpoint);
            channel.setRoutingId(targetRid);
            channel.enableClient(sourceEndpoint); };

        try (ZLinkFrameworkRuntime source =
                 RuntimeTestSupport.startFramework(sourceOptions, new ZLinkJavaBackendAdapterFactory());
             ZLinkFrameworkRuntime target =
                 RuntimeTestSupport.startFramework(targetOptions, new ZLinkJavaBackendAdapterFactory())) {
            ZLinkFrameworkException error = awaitRouteMissingHandlerError(source, targetRid);
            assertTrue(error.getMessage().contains("HANDLER_MISSING"));
            assertTrue(error.getMessage().contains("Missing"));
        }
    }

    @Test
    void routeMesh_nonInitiatorRequestUsesInboundProbeIdentity() {
        String initiatorEndpoint = tcpEndpoint();
        String nonInitiatorEndpoint = tcpEndpoint();
        RoutingId initiatorRid = RoutingId.from("route-a-initiator");
        RoutingId nonInitiatorRid = RoutingId.from("route-z-non-initiator");
        ZLinkInMemoryLocationStore store = new ZLinkInMemoryLocationStore();
        ROUTE_REQUEST_CHANNEL.set(null);

        DefaultZLinkFrameworkOptions initiatorOptions = new DefaultZLinkFrameworkOptions();
        initiatorOptions.addLocationStore(store);
        initiatorOptions.configureLocations().setPollingInterval(Duration.ofMillis(50));
        { var channel = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(initiatorOptions, "route");
            channel.enableServer(initiatorEndpoint);
            channel.enableClient(nonInitiatorEndpoint);
            channel.setRoutingId(initiatorRid);
            channel.addRequestHandler(RouteEchoHandler.class, EchoRequest.class, String.class, "Echo"); };

        DefaultZLinkFrameworkOptions nonInitiatorOptions = new DefaultZLinkFrameworkOptions();
        nonInitiatorOptions.addLocationStore(store);
        nonInitiatorOptions.configureLocations().setPollingInterval(Duration.ofMillis(50));
        { var channel = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(nonInitiatorOptions, "route");
            channel.enableServer(nonInitiatorEndpoint);
            channel.setRoutingId(nonInitiatorRid); };

        try (ZLinkFrameworkRuntime initiator =
                 RuntimeTestSupport.startFramework(initiatorOptions, new ZLinkJavaBackendAdapterFactory());
             ZLinkFrameworkRuntime nonInitiator =
                 RuntimeTestSupport.startFramework(nonInitiatorOptions, new ZLinkJavaBackendAdapterFactory())) {
            assertEquals("route:hello", awaitRouteReply(nonInitiator, initiatorRid));
            assertEquals("route", ROUTE_REQUEST_CHANNEL.get());
        } finally {
            ROUTE_REQUEST_CHANNEL.set(null);
        }
    }

    @Test
    void routeMesh_scannedHandlerGroupRequestSucceeds() {
        String sourceEndpoint = tcpEndpoint();
        String targetEndpoint = tcpEndpoint();
        RoutingId sourceRid = RoutingId.from("route-scanned-source");
        RoutingId targetRid = RoutingId.from("route-scanned-target");

        DefaultZLinkFrameworkOptions sourceOptions = new DefaultZLinkFrameworkOptions();
        { var channel = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(sourceOptions, "route"); channel.enableServer(sourceEndpoint);
            channel.setRoutingId(sourceRid);
            channel.enableClient(targetEndpoint); };

        DefaultZLinkFrameworkOptions targetOptions = new DefaultZLinkFrameworkOptions();
        targetOptions.addHandlersFromPackageOf(ChannelMessagingTest.class);
        { var channel = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(targetOptions, "route"); channel.enableServer(targetEndpoint);
            channel.setRoutingId(targetRid);
            channel.enableClient(sourceEndpoint);
            channel.addHandlerGroup("route-shared"); };

        try (ZLinkFrameworkRuntime source =
                 RuntimeTestSupport.startFramework(sourceOptions, new ZLinkJavaBackendAdapterFactory());
             ZLinkFrameworkRuntime ignoredTarget =
                 RuntimeTestSupport.startFramework(targetOptions, new ZLinkJavaBackendAdapterFactory())) {
            assertEquals("scanned-route:hello", awaitScannedRouteReply(source, targetRid));
        }
    }

    @Test
    void handlerFiltersWrapNodeDirectRequestDispatch() {
        String sourceEndpoint = tcpEndpoint();
        String targetEndpoint = tcpEndpoint();
        RoutingId sourceRid = RoutingId.from("route-filter-source");
        RoutingId targetRid = RoutingId.from("route-filter-target");
        FILTER_PACKET.set(null);
        FILTER_MESH.set(null);
        FILTER_KIND.set(null);

        DefaultZLinkFrameworkOptions sourceOptions = new DefaultZLinkFrameworkOptions();
        { var channel = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(sourceOptions, "route"); channel.enableServer(sourceEndpoint);
            channel.setRoutingId(sourceRid);
            channel.enableClient(targetEndpoint); };

        DefaultZLinkFrameworkOptions targetOptions = new DefaultZLinkFrameworkOptions();
        targetOptions.useFilter(ReplyDecoratingFilter.class);
        { var channel = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(targetOptions, "route"); channel.enableServer(targetEndpoint);
            channel.setRoutingId(targetRid);
            channel.enableClient(sourceEndpoint);
            channel.addRequestHandler(RouteEchoHandler.class, EchoRequest.class, String.class, "Echo"); };

        try (ZLinkFrameworkRuntime source =
                 RuntimeTestSupport.startFramework(sourceOptions, new ZLinkJavaBackendAdapterFactory());
            ZLinkFrameworkRuntime ignoredTarget =
                 RuntimeTestSupport.startFramework(targetOptions, new ZLinkJavaBackendAdapterFactory())) {
            assertEquals("route:hello", awaitRouteReply(source, targetRid));
            assertEquals("Echo", FILTER_PACKET.get());
            assertEquals("route", FILTER_MESH.get());
            assertEquals(
                ZLinkHandlerDispatchKind.NODE_DIRECT_REQUEST,
                FILTER_KIND.get());
        } finally {
            FILTER_PACKET.set(null);
            FILTER_MESH.set(null);
            FILTER_KIND.set(null);
        }
    }

    @Test
    void routeMesh_matchesRepliesByRequestSequenceWhenPacketNameIsShared() {
        String sourceEndpoint = tcpEndpoint();
        String targetEndpoint = tcpEndpoint();
        RoutingId sourceRid = RoutingId.from("route-seq-source");
        RoutingId targetRid = RoutingId.from("route-seq-target");

        DefaultZLinkFrameworkOptions sourceOptions = new DefaultZLinkFrameworkOptions();
        { var channel = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(sourceOptions, "route"); channel.enableServer(sourceEndpoint);
            channel.setRoutingId(sourceRid);
            channel.enableClient(targetEndpoint); };

        DefaultZLinkFrameworkOptions targetOptions = new DefaultZLinkFrameworkOptions();
        { var channel = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(targetOptions, "route"); channel.enableServer(targetEndpoint);
            channel.setRoutingId(targetRid);
            channel.enableClient(sourceEndpoint);
            channel.addRequestHandler(DelayedRouteEchoHandler.class, SharedPacket.class, String.class, "SharedPacket"); };

        try (ZLinkFrameworkRuntime source =
                 RuntimeTestSupport.startFramework(sourceOptions, new ZLinkJavaBackendAdapterFactory());
             ZLinkFrameworkRuntime ignoredTarget =
                 RuntimeTestSupport.startFramework(targetOptions, new ZLinkJavaBackendAdapterFactory())) {
            assertEquals("warmup", awaitSharedRouteReply(source, targetRid, "warmup:1"));

            CompletionStage<String> slow = source.route()
                .requestToNode("route", targetRid, new SharedPacket("slow:40"))
                .timeout(Duration.ofSeconds(3))
                .submit(String.class);
            CompletionStage<String> fast = source.route()
                .requestToNode("route", targetRid, new SharedPacket("fast:1"))
                .timeout(Duration.ofSeconds(3))
                .submit(String.class);

            assertEquals("slow", slow.toCompletableFuture().join());
            assertEquals("fast", fast.toCompletableFuture().join());
        }
    }

    @Test
    void routeMesh_sendByRoutingIdDispatchesToHandler() throws InterruptedException {
        String sourceEndpoint = tcpEndpoint();
        String targetEndpoint = tcpEndpoint();
        RoutingId sourceRid = RoutingId.from("route-send-source");
        RoutingId targetRid = RoutingId.from("route-send-target");
        CountDownLatch latch = new CountDownLatch(1);
        ROUTE_SEND_LATCH.set(latch);
        ROUTE_SEND_MESSAGE.set(null);
        ROUTE_SEND_PACKET.set(null);
        ROUTE_SEND_CHANNEL.set(null);
        ROUTE_SEND_SOURCE.set(null);

        DefaultZLinkFrameworkOptions sourceOptions = new DefaultZLinkFrameworkOptions();
        { var channel = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(sourceOptions, "route"); channel.enableServer(sourceEndpoint);
            channel.setRoutingId(sourceRid);
            channel.enableClient(targetEndpoint); };

        DefaultZLinkFrameworkOptions targetOptions = new DefaultZLinkFrameworkOptions();
        { var channel = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(targetOptions, "route"); channel.enableServer(targetEndpoint);
            channel.setRoutingId(targetRid);
            channel.enableClient(sourceEndpoint);
            channel.addSendHandler(RouteNoticeHandler.class, RouteNotice.class, "Notice"); };

        try (ZLinkFrameworkRuntime source =
                 RuntimeTestSupport.startFramework(sourceOptions, new ZLinkJavaBackendAdapterFactory());
             ZLinkFrameworkRuntime ignoredTarget =
                 RuntimeTestSupport.startFramework(targetOptions, new ZLinkJavaBackendAdapterFactory())) {
            routeSendUntilDelivered(source, targetRid);

            assertTrue(latch.await(1, TimeUnit.SECONDS), "route mesh send was not delivered");
            assertEquals("ping", ROUTE_SEND_MESSAGE.get());
            assertEquals("Notice", ROUTE_SEND_PACKET.get());
            assertEquals("route", ROUTE_SEND_CHANNEL.get());
            assertEquals(sourceRid, ROUTE_SEND_SOURCE.get());
        } finally {
            ROUTE_SEND_LATCH.set(null);
            ROUTE_SEND_MESSAGE.set(null);
            ROUTE_SEND_PACKET.set(null);
            ROUTE_SEND_CHANNEL.set(null);
            ROUTE_SEND_SOURCE.set(null);
        }
    }

    private static String awaitNestedApiReply(ZLinkFrameworkRuntime client) {
        long deadline = System.nanoTime() + Duration.ofSeconds(4).toNanos();
        RuntimeException lastFailure = null;
        while (System.nanoTime() < deadline) {
            try {
                return client.client()
                    .requestToChannel("api", new NestedApi("hello"))
                    .timeout(Duration.ofMillis(200))
                    .submit(String.class)
                    .toCompletableFuture()
                    .join();
            } catch (RuntimeException ex) {
                lastFailure = ex;
                Thread.onSpinWait();
            }
        }
        throw new AssertionError("nested api route request did not succeed", lastFailure);
    }

    private static String awaitSpotAttachedChannelReply(ZLinkFrameworkRuntime runtime) {
        runtime.spotManager()
            .getOrCreate("outbound-channel-spot", "OutboundChannelSpot")
            .submit()
            .toCompletableFuture()
            .join();
        ZLinkSpotContext context = Objects.requireNonNull(OutboundChannelSpot.CONTEXT.get());
        long deadline = System.nanoTime() + Duration.ofSeconds(4).toNanos();
        RuntimeException lastFailure = null;
        while (System.nanoTime() < deadline) {
            try {
                return context.outbound()
                    .requestToChannel("api", new SpotApi("hello"))
                    .timeout(Duration.ofMillis(200))
                    .submit(String.class)
                    .toCompletableFuture()
                    .join();
            } catch (RuntimeException ex) {
                lastFailure = new RuntimeException(describeZlinkFailure(ex), ex);
                Thread.onSpinWait();
            }
        }
        throw new AssertionError("SPOT attached channel request did not succeed", lastFailure);
    }

    private static String describeZlinkFailure(Throwable error) {
        Throwable current = error;
        while (current != null) {
            if (current instanceof systems.zlink.contracts.errors.ZlinkRequestException request) {
                return "request result=" + request.getResult()
                    + ", errno=" + request.getNativeErrno();
            }
            if (current instanceof systems.zlink.contracts.errors.ZlinkSubmitException submit) {
                return "submit result=" + submit.getResult()
                    + ", errno=" + submit.getNativeErrno();
            }
            current = current.getCause();
        }
        return error.toString();
    }

    private static void publishUntilDelivered(ZLinkFrameworkRuntime publisher) {
        long deadline = System.nanoTime() + Duration.ofSeconds(10).toNanos();
        while (System.nanoTime() < deadline && FANOUT_LATCH.get().getCount() > 0) {
            publisher.fanout()
                .publish("events", "score", new ScoreChanged("home:1"))
                .submit();
            Thread.onSpinWait();
        }
    }

    private static String publishUntilCommonFanoutSequence(ZLinkFrameworkRuntime publisher) {
        long deadline = System.nanoTime() + Duration.ofSeconds(4).toNanos();
        int sequence = 0;
        String common = null;
        while (System.nanoTime() < deadline) {
            common = commonFanoutSequence();
            if (common != null) {
                return common;
            }
            String value = "seq:" + sequence++;
            publisher.fanout()
                .publish("sequence", "score", new FanoutSequence(value))
                .submit();
            Thread.onSpinWait();
        }
        common = commonFanoutSequence();
        if (common != null) {
            return common;
        }
        throw new AssertionError(
            "fanout sequence was not delivered to all subscribers: first="
                + FANOUT_SEQUENCE_ONE
                + ", second="
                + FANOUT_SEQUENCE_TWO
                + ", third="
                + FANOUT_SEQUENCE_THREE);
    }

    private static String commonFanoutSequence() {
        for (String value : FANOUT_SEQUENCE_ONE) {
            if (FANOUT_SEQUENCE_TWO.contains(value) && FANOUT_SEQUENCE_THREE.contains(value)) {
                return value;
            }
        }
        return null;
    }

    private static void publishManualRegistrationUntilDelivered(
        ZLinkFrameworkRuntime publisher,
        String packetName,
        String value) {
        long deadline = System.nanoTime() + Duration.ofSeconds(3).toNanos();
        while (System.nanoTime() < deadline && MANUAL_REG_PUBLISH_LATCH.get().getCount() > 0) {
            publisher.fanout()
                .publish("manual-events", "manual", new ManualEvent(value))
                .submit();
            Thread.onSpinWait();
        }
    }

    private static void publishManualRegistrationUntilObserved(
        ZLinkFrameworkRuntime publisher,
        List<ZLinkMessageFlowEvent> observedErrors,
        String packetName) {
        long deadline = System.nanoTime() + Duration.ofSeconds(3).toNanos();
        while (System.nanoTime() < deadline && observedErrors.stream().noneMatch(error ->
            error.messageKind() == ZLinkDispatchMessageKind.PUBLISH
                && error.errorReason() == ZLinkDispatchErrorReason.HANDLER_MISSING
                && packetName.equals(error.packetName()))) {
            publisher.fanout()
                .publish("manual-events", "manual", new ManualMissingEvent("missing-event"))
                .submit();
            Thread.onSpinWait();
        }
    }

    private static void sendUntilDelivered(ZLinkFrameworkRuntime runtime) {
        long deadline = System.nanoTime() + Duration.ofSeconds(3).toNanos();
        while (System.nanoTime() < deadline && SEND_LATCH.get().getCount() > 0) {
            runtime.client()
                .sendToChannel("profile", new ProfileChanged("changed"))
                .submit();
            Thread.onSpinWait();
        }
    }

    private static String awaitRouteReply(ZLinkFrameworkRuntime source, RoutingId targetRid) {
        long deadline = System.nanoTime() + Duration.ofSeconds(10).toNanos();
        RuntimeException lastFailure = null;
        while (System.nanoTime() < deadline) {
            try {
                return source.route()
                    .requestToNode("route", targetRid, new EchoRequest("hello"))
                    .timeout(Duration.ofMillis(500))
                    .submit(String.class)
                    .toCompletableFuture()
                    .join();
            } catch (RuntimeException ex) {
                lastFailure = ex;
                Thread.onSpinWait();
            }
        }
        throw new AssertionError("route mesh request did not succeed", lastFailure);
    }

    private static ZLinkFrameworkException awaitRouteMissingHandlerError(
        ZLinkFrameworkRuntime source,
        RoutingId targetRid) {
        long deadline = System.nanoTime() + Duration.ofSeconds(10).toNanos();
        RuntimeException lastFailure = null;
        while (System.nanoTime() < deadline) {
            try {
                source.route()
                    .requestToNode("route", targetRid, new MissingRouteRequest("hello"))
                    .timeout(Duration.ofMillis(100))
                    .submit(String.class)
                    .toCompletableFuture()
                    .join();
            } catch (CompletionException ex) {
                if (ex.getCause() instanceof ZLinkFrameworkException frameworkError
                    && frameworkError.getMessage().contains("HANDLER_MISSING")) {
                    return frameworkError;
                }
                lastFailure = ex;
            } catch (RuntimeException ex) {
                lastFailure = ex;
            }
            Thread.onSpinWait();
        }
        throw new AssertionError("route mesh missing handler did not return framework error", lastFailure);
    }

    private static String awaitScannedRouteReply(ZLinkFrameworkRuntime source, RoutingId targetRid) {
        long deadline = System.nanoTime() + Duration.ofSeconds(3).toNanos();
        RuntimeException lastFailure = null;
        while (System.nanoTime() < deadline) {
            try {
                return source.route()
                    .requestToNode("route", targetRid, new StringPacket("hello"))
                    .timeout(Duration.ofMillis(100))
                    .submit(String.class)
                    .toCompletableFuture()
                    .join();
            } catch (RuntimeException ex) {
                lastFailure = ex;
                Thread.onSpinWait();
            }
        }
        throw new AssertionError("scanned route mesh request did not succeed", lastFailure);
    }

    private static SpotHandle awaitSpotHandle(
        SpotHandleResolver resolver,
        String spotId) {
        long deadline = System.nanoTime() + Duration.ofSeconds(3).toNanos();
        while (System.nanoTime() < deadline) {
            var handle = resolver.resolveSpotHandle(spotId).toCompletableFuture().join();
            if (handle.isPresent()) {
                return handle.get();
            }
            Thread.onSpinWait();
        }
        throw new AssertionError("spot handle was not published: " + spotId);
    }

    private static String awaitSharedRouteReply(
        ZLinkFrameworkRuntime source,
        RoutingId targetRid,
        String message) {
        long deadline = System.nanoTime() + Duration.ofSeconds(3).toNanos();
        RuntimeException lastFailure = null;
        while (System.nanoTime() < deadline) {
            try {
                return source.route()
                    .requestToNode("route", targetRid, new SharedPacket(message))
                    .timeout(Duration.ofMillis(100))
                    .submit(String.class)
                    .toCompletableFuture()
                    .join();
            } catch (RuntimeException ex) {
                lastFailure = ex;
                Thread.onSpinWait();
            }
        }
        throw new AssertionError("shared packet route request did not succeed", lastFailure);
    }

    private static void routeSendUntilDelivered(ZLinkFrameworkRuntime source, RoutingId targetRid) {
        long deadline = System.nanoTime() + Duration.ofSeconds(3).toNanos();
        while (System.nanoTime() < deadline && ROUTE_SEND_LATCH.get().getCount() > 0) {
            source.route()
                .sendToNode("route", targetRid, new RouteNotice("ping"))
                .submit();
            Thread.onSpinWait();
        }
    }

    private static String tcpEndpoint() {
        return "tcp://127.0.0.1:" + nextPort();
    }

    private static <T> T awaitChannelReply(
        ZLinkFrameworkRuntime runtime,
        String channelName,
        Object request,
        Class<T> replyType) {
        long deadline = System.nanoTime() + Duration.ofSeconds(10).toNanos();
        RuntimeException lastFailure = null;
        while (System.nanoTime() < deadline) {
            try {
                return runtime.client()
                    .requestToChannel(channelName, request)
                    .timeout(Duration.ofMillis(300))
                    .submit(replyType)
                    .toCompletableFuture()
                    .join();
            } catch (RuntimeException failure) {
                lastFailure = failure;
                try {
                    Thread.sleep(10);
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    throw new AssertionError(interrupted);
                }
            }
        }
        throw new AssertionError(
            "client/server channel did not become ready: " + channelName,
            lastFailure);
    }

    private static systems.zlink.framework.configuration
        .ZLinkClientServerChannelServerBuilder listenClientServer(
            DefaultZLinkFrameworkOptions options,
            String channelName,
            String endpoint) {
        URI uri = URI.create(endpoint);
        return options.addClientServerChannel(channelName)
            .server()
            .setBindHost(uri.getHost())
            .listen(uri.getPort());
    }

    private static String dispatchLogLine(ZLinkMessageFlowEvent error) {
        return "dispatch-error"
            + " surface=" + error.surface()
            + " messageKind=" + error.messageKind()
            + " reason=" + error.errorReason()
            + " action=" + error.errorAction()
            + " packetName=" + error.packetName()
            + " channelName=" + error.channelName()
            + " correlationId=" + error.correlationId()
            + System.lineSeparator();
    }

    private static String waitForFileLog(Path path, String marker, int expected) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
        while (System.nanoTime() < deadline) {
            String text = Files.readString(path, StandardCharsets.UTF_8);
            if (countOccurrences(text, marker) >= expected) {
                return text;
            }
            Thread.sleep(25);
        }
        return Files.readString(path, StandardCharsets.UTF_8);
    }

    private static boolean hasDispatchError(
        List<ZLinkMessageFlowEvent> errors,
        ZLinkDispatchMessageKind kind,
        ZLinkDispatchErrorReason reason,
        ZLinkDispatchErrorAction action,
        String packetName,
        String channelName) {
        return errors.stream().anyMatch(error ->
            error.surface() == ZLinkDispatchErrorSurface.CHANNEL
                && error.messageKind() == kind
                && error.errorReason() == reason
                && error.errorAction() == action
                && packetName.equals(error.packetName())
                && channelName.equals(error.channelName()));
    }

    private static boolean waitForManualRegistrationErrors(
        List<ZLinkMessageFlowEvent> errors)
        throws InterruptedException {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
        while (System.nanoTime() < deadline) {
            if (hasDispatchError(
                    errors,
                    ZLinkDispatchMessageKind.REQUEST,
                    ZLinkDispatchErrorReason.HANDLER_MISSING,
                    ZLinkDispatchErrorAction.REPLY_ERROR,
                    "ManualMissingReq",
                    "manual-reg")
                && hasDispatchError(
                    errors,
                    ZLinkDispatchMessageKind.SEND,
                    ZLinkDispatchErrorReason.HANDLER_MISSING,
                    ZLinkDispatchErrorAction.DROP,
                    "ManualMissingCommand",
                    "manual-reg")
                && hasDispatchError(
                    errors,
                    ZLinkDispatchMessageKind.PUBLISH,
                    ZLinkDispatchErrorReason.HANDLER_MISSING,
                    ZLinkDispatchErrorAction.DROP,
                    "ManualMissingEvent",
                    "manual-events")) {
                return true;
            }
            Thread.sleep(25);
        }
        return false;
    }

    private static int countOccurrences(String text, String marker) {
        int count = 0;
        int index = 0;
        while ((index = text.indexOf(marker, index)) >= 0) {
            count++;
            index += marker.length();
        }
        return count;
    }

    private static int nextPort() {
        for (int attempt = 0; attempt < 200; attempt++) {
            int port = NEXT_PORT.getAndIncrement();
            if (isBindable(port)) {
                return port;
            }
        }
        throw new IllegalStateException("failed to allocate tcp port");
    }

    private static String message(String value) {
        return value;
    }

    private static boolean isBindable(int port) {
        try (ServerSocket server = new ServerSocket(port)) {
            server.setReuseAddress(false);
            return true;
        } catch (IOException ignored) {
            return false;
        }
    }

    public static final class EchoHandler implements ZLinkRequestHandler<EchoRequest, String> {
        @Override
        public CompletionStage<String> handle(EchoRequest request, ZLinkMessageContext context) {
            return CompletableFuture.completedFuture(request.value());
        }
    }

    public static final class OutboundChannelSpot implements ZLinkSpot<ZLinkActor> {
        static final AtomicReference<ZLinkSpotContext> CONTEXT = new AtomicReference<>();
        static final AtomicReference<SpotHandleResolver> HANDLES = new AtomicReference<>();
        private final ZLinkSpotContext context;

        public OutboundChannelSpot(ZLinkSpotContext context, SpotHandleResolver handles) {
            this.context = context;
            CONTEXT.set(context);
            HANDLES.set(handles);
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) { return CompletableFuture.completedFuture(null); }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) { return CompletableFuture.completedFuture(null); }
    }

    public static final class RemoteStateSpot implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;

        public RemoteStateSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) { return CompletableFuture.completedFuture(null); }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) { return CompletableFuture.completedFuture(null); }

        @Override
        public void configure() {
            context.handlers().addHandler(RemoteStateHandler.class);
        }
    }

    public static final class RemoteStateHandler {
        @ZLinkSpotRequest
        public CompletionStage<SpotEgressReply> handle(
            RemoteStateSpot spot,
            SpotEgressRequest request) {
            return CompletableFuture.completedFuture(new SpotEgressReply(
                spot.context().spotId().toString(),
                spot.context().nodeRid().toString(),
                "pong:" + request.value()));
        }
    }

    public record SpotEgressRequest(String value) {
    }

    @ZLinkPacket("Missing")
    public record MissingRouteRequest(String value) {
    }

    @ZLinkPacket("ProfileChanged")
    public record ProfileChanged(String value) {
    }

    @ZLinkPacket("ScoreChanged")
    public record ScoreChanged(String value) {
    }

    @ZLinkPacket("FanoutSequence")
    public record FanoutSequence(String value) {
    }

    @ZLinkPacket("Notice")
    public record RouteNotice(String value) {
    }

    @ZLinkPacket("Echo") public record EchoRequest(String value) { }
    @ZLinkPacket("ThrowReq") public record ThrowRequest(String value) { }
    @ZLinkPacket("MissingReq") public record MissingRequest(String value) { }
    @ZLinkPacket("MissingCommand") public record MissingCommand(String value) { }
    @ZLinkPacket("ManualRegisteredReq") public record ManualRequest(String value) { }
    @ZLinkPacket("ManualRegisteredCommand") public record ManualCommand(String value) { }
    @ZLinkPacket("ManualRegisteredEvent") public record ManualEvent(String value) { }
    @ZLinkPacket("ManualMissingReq") public record ManualMissingRequest(String value) { }
    @ZLinkPacket("ManualMissingCommand") public record ManualMissingCommand(String value) { }
    @ZLinkPacket("ManualMissingEvent") public record ManualMissingEvent(String value) { }
    @ZLinkPacket("String") public record StringPacket(String value) { }
    @ZLinkPacket("AnnotatedEcho") public record AnnotatedEcho(String value) { }
    @ZLinkPacket("SharedPacket") public record SharedPacket(String value) { }
    @ZLinkPacket("NestedApi") public record NestedApi(String value) { }
    @ZLinkPacket("SpotApi") public record SpotApi(String value) { }
    @ZLinkPacket("NestedRoute") public record NestedRoute(String value) { }

    public record SpotEgressReply(
        String spotId,
        String nodeRid,
        String value) {
    }


    public static final class ThrowingRequestHandler implements ZLinkRequestHandler<ThrowRequest, String> {
        @Override
        public CompletionStage<String> handle(ThrowRequest request, ZLinkMessageContext context) {
            return CompletableFuture.failedFuture(new IllegalStateException("DERR-007 handler exception"));
        }
    }

    @ZLinkPacket("DecodeReq")
    public record DecodePayload(String value) {
    }

    @ZLinkPacket("JsonCodecProbe")
    public record JsonCodecProbe(
        String name,
        int revision,
        String optionalLabel,
        List<String> tags,
        List<JsonCodecChild> children,
        JsonCodecChild nested) {
        public JsonCodecProbe {
            tags = List.copyOf(Objects.requireNonNull(tags, "tags"));
            children = List.copyOf(Objects.requireNonNull(children, "children"));
        }
    }

    public record JsonCodecChild(String name, int rank) {
    }

    public static final class DecodeProbeHandler implements ZLinkRequestHandler<DecodePayload, String> {
        static final AtomicInteger invocations = new AtomicInteger();

        @Override
        public CompletionStage<String> handle(DecodePayload request, ZLinkMessageContext context) {
            invocations.incrementAndGet();
            return CompletableFuture.completedFuture("decode:" + request.value());
        }
    }

    public static final class JsonCodecEchoHandler implements ZLinkRequestHandler<JsonCodecProbe, JsonCodecProbe> {
        @Override
        public CompletionStage<JsonCodecProbe> handle(JsonCodecProbe request, ZLinkMessageContext context) {
            return CompletableFuture.completedFuture(request);
        }
    }

    public static final class ManualRegistrationRequestHandler implements ZLinkRequestHandler<ManualRequest, String> {
        @Override
        public CompletionStage<String> handle(ManualRequest request, ZLinkMessageContext context) {
            return CompletableFuture.completedFuture("manual:" + request.value());
        }
    }

    public static final class ManualRegistrationCommandHandler implements ZLinkSendHandler<ManualCommand> {
        @Override
        public CompletionStage<Void> handle(ManualCommand message, ZLinkMessageContext context) {
            MANUAL_REG_SEND_MESSAGE.set(message.value());
            MANUAL_REG_SEND_PACKET.set(context.packetName());
            MANUAL_REG_SEND_CHANNEL.set(context.channelName().orElse(""));
            MANUAL_REG_SEND_LATCH.get().countDown();
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ManualRegistrationPublishHandler implements ZLinkFanoutHandler<ManualEvent> {
        @Override
        public CompletionStage<Void> handle(ManualEvent message, ZLinkPublishMessageContext context) {
            MANUAL_REG_PUBLISH_MESSAGE.set(message.value());
            MANUAL_REG_PUBLISH_TOPIC.set(context.topic());
            MANUAL_REG_PUBLISH_CHANNEL.set(context.channelName().orElse(""));
            MANUAL_REG_PUBLISH_LATCH.get().countDown();
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ReplyDecoratingFilter implements ZLinkHandlerFilter {
        @Override
        public <T> CompletionStage<T> invoke(
            ZLinkHandlerFilterContext context,
            ZLinkHandlerFilterNext<T> next) {
            FILTER_PACKET.set(context.packetName());
            FILTER_CHANNEL.set(context.channelName().orElse(""));
            FILTER_MESH.set(context.meshName().orElse(""));
            FILTER_KIND.set(context.dispatchKind());
            return next.invoke();
        }
    }

    @ZLinkHandlerGroup("scanned-profile")
    public static final class ScannedEchoHandler implements ZLinkRequestHandler<StringPacket, String> {
        @Override
        public CompletionStage<String> handle(StringPacket request, ZLinkMessageContext context) {
            return CompletableFuture.completedFuture("scanned:" + request.value());
        }
    }

    public static final class ProfileChangedHandler implements ZLinkSendHandler<ProfileChanged> {
        @Override
        public CompletionStage<Void> handle(ProfileChanged message, ZLinkMessageContext context) {
            SEND_MESSAGE.set(message.value());
            SEND_PACKET.set(context.packetName());
            SEND_CHANNEL.set(context.channelName().orElse(""));
            SEND_LATCH.get().countDown();
            return CompletableFuture.completedFuture(null);
        }
    }

    @ZLinkHandlerGroup("annotated-profile")
    public static final class AnnotatedProfileHandlers {
        @ZLinkRequest(packetName = "AnnotatedEcho")
        public CompletionStage<String> echo(AnnotatedEcho request) {
            return CompletableFuture.completedFuture("annotated:" + request.value());
        }

        @ZLinkSend(packetName = "ProfileChanged")
        public CompletionStage<Void> profileChanged(ProfileChanged message) {
            SEND_MESSAGE.set(message.value());
            SEND_PACKET.set("ProfileChanged");
            SEND_LATCH.get().countDown();
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ScoreChangedHandler implements ZLinkFanoutHandler<ScoreChanged> {
        @Override
        public CompletionStage<Void> handle(ScoreChanged message, ZLinkPublishMessageContext context) {
            FANOUT_MESSAGE.set(message.value());
            FANOUT_TOPIC.set(context.topic());
            FANOUT_CHANNEL.set(context.channelName().orElse(""));
            FANOUT_LATCH.get().countDown();
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class FanoutSequenceOneHandler implements ZLinkFanoutHandler<FanoutSequence> {
        @Override
        public CompletionStage<Void> handle(FanoutSequence message, ZLinkPublishMessageContext context) {
            FANOUT_SEQUENCE_ONE.add(message.value());
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class FanoutSequenceTwoHandler implements ZLinkFanoutHandler<FanoutSequence> {
        @Override
        public CompletionStage<Void> handle(FanoutSequence message, ZLinkPublishMessageContext context) {
            FANOUT_SEQUENCE_TWO.add(message.value());
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class FanoutSequenceThreeHandler implements ZLinkFanoutHandler<FanoutSequence> {
        @Override
        public CompletionStage<Void> handle(FanoutSequence message, ZLinkPublishMessageContext context) {
            FANOUT_SEQUENCE_THREE.add(message.value());
            return CompletableFuture.completedFuture(null);
        }
    }

    @ZLinkHandlerGroup("annotated-events")
    public static final class AnnotatedEventHandlers {
        @ZLinkPublish(packetName = "ScoreChanged")
        public CompletionStage<Void> scoreChanged(ScoreChanged message) {
            FANOUT_MESSAGE.set(message.value());
            FANOUT_TOPIC.set("score");
            FANOUT_LATCH.get().countDown();
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class RouteEchoHandler implements ZLinkRouteRequestHandler<EchoRequest, String> {
        @Override
        public CompletionStage<String> handle(EchoRequest request, ZLinkRouteMessageContext context) {
            ROUTE_REQUEST_CHANNEL.set(context.meshName().orElse(""));
            return CompletableFuture.completedFuture("route:" + request.value());
        }
    }

    public static final class NestedRouteApiHandler implements ZLinkRequestHandler<NestedApi, String> {
        private final ZLinkRouteClient routes;

        public NestedRouteApiHandler(ZLinkRouteClient routes) {
            this.routes = routes;
        }

        @Override
        public CompletionStage<String> handle(NestedApi request, ZLinkMessageContext context) {
            return routes.requestToNode("route", RoutingId.from("nested-play-route"), new NestedRoute(request.value()))
                .timeout(Duration.ofMillis(200))
                .submit(String.class);
        }
    }

    @ZLinkHandlerGroup("route-shared")
    public static final class ScannedRouteEchoHandler
        implements ZLinkRouteRequestHandler<StringPacket, String> {
        @Override
        public CompletionStage<String> handle(
            StringPacket request,
            ZLinkRouteMessageContext context) {
            return CompletableFuture.completedFuture("scanned-route:" + request.value());
        }
    }

    public static final class DelayedRouteEchoHandler implements ZLinkRouteRequestHandler<SharedPacket, String> {
        @Override
        public CompletionStage<String> handle(SharedPacket request, ZLinkRouteMessageContext context) {
            String[] parts = request.value().split(":", 2);
            String value = parts[0];
            long delayMillis = parts.length == 2 ? Long.parseLong(parts[1]) : 0;
            return CompletableFuture.supplyAsync(
                () -> value,
                CompletableFuture.delayedExecutor(delayMillis, TimeUnit.MILLISECONDS));
        }
    }

    public static final class RouteNoticeHandler implements ZLinkRouteSendHandler<RouteNotice> {
        @Override
        public CompletionStage<Void> handle(RouteNotice message, ZLinkRouteMessageContext context) {
            ROUTE_SEND_MESSAGE.set(message.value());
            ROUTE_SEND_PACKET.set(context.packetName());
            ROUTE_SEND_CHANNEL.set(context.meshName().orElse(""));
            ROUTE_SEND_SOURCE.set(context.sourceNodeRid());
            ROUTE_SEND_LATCH.get().countDown();
            return CompletableFuture.completedFuture(null);
        }
    }
}
