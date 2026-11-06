package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.RepeatedTest;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.handlers.ZLinkPacket;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;

import java.lang.reflect.Proxy;
import java.nio.ByteBuffer;
import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletionException;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkRegisteredEnvelopeEncodingTest {
    @ZLinkPacket("probe-message")
    record Request(String value) {}

    @RepeatedTest(100)
    void actualRequestUsesRegisteredNamesAndDynamicNamesKeepWriterBytes() {
        for (boolean nodeDirect : List.of(false, true)) {
            var options = new DefaultZLinkFrameworkOptions();
            options.setDefaultRequestTimeout(Duration.ofSeconds(1));
            var registration = options.registration();
            var mesh =
                    new systems.zlink.framework.runtime.mesh.MeshNodeRegistration(
                            nodeDirect ? "probe-channel" : "mesh");
            if (!nodeDirect) mesh.channelName("probe-channel").client();
            registration.meshNodes().add(mesh);
            registration.codecs().contentTypeFor(Request.class);
            var captured = new AtomicReference<byte[]>();
            var stop = new IllegalStateException("encode captured");
            var context =
                    (systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext)
                            Proxy.newProxyInstance(
                                    getClass().getClassLoader(),
                                    new Class<?>[] {
                                        systems.zlink.framework.runtime.internal.backend
                                                .ZLinkBackendContext.class
                                    },
                                    (proxy, method, arguments) -> {
                                        assertEquals("close", method.getName());
                                        return null;
                                    });
            var backend =
                    (systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter)
                            Proxy.newProxyInstance(
                                    getClass().getClassLoader(),
                                    new Class<?>[] {
                                        systems.zlink.framework.runtime.internal.backend
                                                .ZLinkChannelBackendAdapter.class
                                    },
                                    (proxy, method, arguments) -> {
                                        assertEquals("createContext", method.getName());
                                        return context;
                                    });
            var node =
                    (systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode)
                            Proxy.newProxyInstance(
                                    getClass().getClassLoader(),
                                    new Class<?>[] {
                                        systems.zlink.framework.runtime.internal.backend
                                                .ZLinkInternalSpotNode.class
                                    },
                                    (proxy, method, arguments) -> {
                                        if ((method.getName().equals("requestToNode")
                                                || method.getName().equals("requestToChannel"))) {
                                            @SuppressWarnings("unchecked")
                                            List<Message> parts = (List<Message>) arguments[2];
                                            captured.set(parts.getFirst().toByteArray());
                                            throw stop;
                                        }
                                        if (method.getName().equals("sourceTransportLane")
                                                || method.getName().equals("operationRegistry")
                                                || method.getName().equals("close")) return null;
                                        if (method.getName().equals("name")) return "probe-channel";
                                        if (method.isDefault())
                                            return java.lang.reflect.InvocationHandler
                                                    .invokeDefault(proxy, method, arguments);
                                        throw new AssertionError("unexpected node call: " + method);
                                    });
            try (var runtime =
                    new ZLinkChannelRuntime(
                            backend,
                            registration,
                            new systems.zlink.framework.runtime.messaging
                                    .ZLinkJsonMessageSerializer())) {
                runtime.registerSpotRouterNode("probe-channel", node);
                for (String packet : List.of("probe-message", "dynamic-\"\\\n😀")) {
                    System.setProperty("envelope.escapes", "0");
                    var call =
                            nodeDirect
                                    ? runtime.requestToNode(
                                            "probe-channel",
                                            RoutingId.from("peer"),
                                            new Request("payload"))
                                    : runtime.requestToChannel(
                                            "probe-channel", new Request("payload"));
                    var selected =
                            packet.equals("probe-message")
                                    ? call
                                    : nodeDirect
                                            ? ((RouteRequestCall) call).packetName(packet)
                                            : ((ChannelRequestCall) call).packetName(packet);
                    var failure =
                            assertThrows(
                                    CompletionException.class,
                                    () ->
                                            selected.submit(Request.class)
                                                    .toCompletableFuture()
                                                    .join());
                    Throwable cause = failure;
                    while (cause.getCause() != null) cause = cause.getCause();
                    assertSame(stop, cause);
                    if (Boolean.getBoolean("envelope.probe"))
                        assertEquals(0, Integer.getInteger("envelope.escapes", 0));
                    byte[] bytes = captured.get();
                    assertNotNull(bytes);
                    var header = ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(bytes), true);
                    assertEquals(packet, header.messageName());
                    assertArrayEquals(ZLinkChannelEnvelope.encodeHeaderBytes(header), bytes);
                    if (packet.equals("probe-message") && Boolean.getBoolean("envelope.probe"))
                        assertEquals(6, Integer.getInteger("envelope.escapes", 0));
                }
            }
        }
    }

    @RepeatedTest(100)
    void successfulReplyUsesRegisteredNamesForDirectAndRouterPaths() {
        var options = new DefaultZLinkFrameworkOptions();
        var registration = options.registration();
        registration
                .channels()
                .add(new ChannelRegistration("probe-channel", ChannelKind.ROUTE_MESH));
        var selections = new java.util.concurrent.atomic.AtomicInteger();
        registration
                .codecs()
                .addSerializer(
                        "application/custom",
                        new systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer(),
                        ignored -> {
                            selections.incrementAndGet();
                            throw new IllegalStateException(
                                    "outgoing selection is not a reply decision");
                        });
        var handler =
                new ChannelRequestHandlerRegistration(
                        EchoHandler.class,
                        UnannotatedRequest.class,
                        UnannotatedRequest.class,
                        "probe-message");
        var reporter =
                new ZLinkChannelDispatchReporter(
                        new systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter(
                                registration.dispatchOptions(),
                                ZLinkHandlerActivator.reflection(),
                                Runnable::run),
                        registration.codecs());
        var request =
                new ZLinkChannelEnvelope.Header(
                        1,
                        "probe-channel",
                        "probe-message",
                        "application/json",
                        "response-counter",
                        null,
                        null,
                        null,
                        null,
                        null,
                        java.util.Map.of(),
                        null,
                        null);
        for (boolean direct : List.of(false, true)) {
            var captured = new AtomicReference<byte[]>();
            var router =
                    (ZLinkBackendRouterSocket)
                            Proxy.newProxyInstance(
                                    getClass().getClassLoader(),
                                    new Class<?>[] {ZLinkBackendRouterSocket.class},
                                    (proxy, method, arguments) -> {
                                        assertEquals("reply", method.getName());
                                        @SuppressWarnings("unchecked")
                                        List<Message> parts = (List<Message>) arguments[2];
                                        captured.set(parts.getFirst().toByteArray());
                                        return null;
                                    });
            try (var received =
                    new systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived(
                            null,
                            Optional.of(RoutingId.from("peer")),
                            Optional.empty(),
                            Optional.of(1L),
                            null,
                            null,
                            List.of(),
                            direct ? parts -> captured.set(parts.getFirst().toByteArray()) : null,
                            () -> {},
                            "application/json")) {
                System.setProperty("envelope.escapes", "0");
                reporter.replyRegisteredPayloadAndClose(
                        router,
                        received,
                        request,
                        Message.from("reply"),
                        registration.channels().getFirst().envelopeNames(),
                        handler.envelopeNames());
                if (Boolean.getBoolean("envelope.probe"))
                    assertEquals(0, Integer.getInteger("envelope.escapes", 0));
                assertEquals(0, selections.get());
                assertArrayEquals(
                        ZLinkChannelEnvelope.encodeHeaderBytes(ZLinkChannelEnvelope.reply(request)),
                        captured.get());
                if (Boolean.getBoolean("envelope.probe"))
                    assertEquals(6, Integer.getInteger("envelope.escapes", 0));
            }
        }
    }

    @RepeatedTest(100)
    void uncachedTypesUseDynamicMessageWriterWithoutPreparingAgain() {
        var codecs = new DefaultZLinkFrameworkOptions().registration().codecs();
        for (Class<?> base : List.of(int.class, long.class, short.class, byte.class, char.class)) {
            Class<?> type = base;
            for (int rank = 1; rank <= 255; rank++) {
                type = type.arrayType();
                codecs.contentTypeFor(type);
            }
        }
        var channelNames = new ZLinkChannelEnvelope.PreparedNames("probe-channel", null, null);
        System.setProperty("envelope.escapes", "0");
        var names = codecs.sendSelectionFor(Request.class).names();
        if (Boolean.getBoolean("envelope.probe"))
            assertEquals(0, Integer.getInteger("envelope.escapes", 0));
        var header =
                new ZLinkChannelEnvelope.Header(
                        1,
                        "probe-channel",
                        "probe-message",
                        "application/json",
                        "uncached-counter",
                        null,
                        null,
                        null,
                        null,
                        null,
                        java.util.Map.of(),
                        null,
                        null);
        var parts =
                ZLinkChannelEnvelope.encode(
                        header, Message.from("payload"), channelNames, names, names);
        try {
            if (Boolean.getBoolean("envelope.probe"))
                assertEquals(2, Integer.getInteger("envelope.escapes", 0));
            assertArrayEquals(
                    ZLinkChannelEnvelope.encodeHeaderBytes(header), parts.getFirst().toByteArray());
        } finally {
            ZLinkFrameworkMultipartView.closeAll(parts);
        }
    }

    @RepeatedTest(100)
    void replyIsReleasedWhenHeaderEncodingFails() {
        var registration = new DefaultZLinkFrameworkOptions().registration();
        var reporter = new ZLinkChannelDispatchReporter(null, registration.codecs());
        var header =
                new ZLinkChannelEnvelope.Header(
                        1,
                        "probe-channel",
                        "probe-message",
                        "application/json",
                        "failure-counter",
                        null,
                        null,
                        null,
                        null,
                        null,
                        java.util.Map.of(),
                        "invalid-flow",
                        systems.zlink.framework.monitoring.ZLinkFlowOrigin.APPLICATION);
        try (var received =
                        new systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived(
                                null,
                                Optional.empty(),
                                Optional.empty(),
                                Optional.empty(),
                                null,
                                null,
                                List.of(),
                                parts -> {
                                    throw new AssertionError(
                                            "invalid header must precede reply admission");
                                },
                                () -> {},
                                "application/json");
                Message reply = Message.from("reply")) {
            assertThrows(
                    systems.zlink.framework.errors.ZLinkFrameworkException.class,
                    () ->
                            reporter.replyRegisteredPayloadAndClose(
                                    null, received, header, reply, null, null));
            assertTrue(reply.empty(), "failed header encoding must release the reply owner");
        }
    }

    record UnannotatedRequest(String value) {}

    public static final class EchoHandler
            implements systems.zlink.framework.channels.ZLinkRequestHandler<
                    UnannotatedRequest, UnannotatedRequest> {
        @Override
        public java.util.concurrent.CompletionStage<UnannotatedRequest> handle(
                UnannotatedRequest request, systems.zlink.framework.ZLinkMessageContext context) {
            return java.util.concurrent.CompletableFuture.completedFuture(request);
        }
    }
}
