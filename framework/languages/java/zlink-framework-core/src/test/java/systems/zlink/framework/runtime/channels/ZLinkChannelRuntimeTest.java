package systems.zlink.framework.runtime.channels;
import org.junit.jupiter.api.Assertions;
import java.util.LinkedHashMap;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;
import systems.zlink.framework.runtime.host.ZLinkTestAdmissionFactory;
import systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology;
import systems.zlink.framework.runtime.messaging.OneWayTestStatus;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorReply;
import systems.zlink.framework.spots.ZLinkSpotKind;

import systems.zlink.framework.spots.SpotHandles;

import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.lang.reflect.Method;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.ZLinkMessageSerializer;

import systems.zlink.framework.configuration.ZLinkEndpointConnections;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.runtime.internal.spots.ZLinkInstanceSpotCallRuntime;

final class ZLinkChannelRuntimeTest {
    private static ZLinkHandlerActivator handlers() {
        SpotTransportAddressResolver resolver = spotId ->
            CompletableFuture.completedFuture(Optional.of(
                new SpotTransportAddress(
                    "play.route",
                    RoutingId.from("play-node"),
                    spotId,
                    1L,
                    ZLinkSpotKind.USER)));
        return handlers(resolver);
    }

    private static ZLinkHandlerActivator handlers(
        SpotTransportAddressResolver resolver) {
        return ZLinkHandlerActivator.services()
            .add(SpotTransportAddressResolver.class, resolver);
    }

    @Test
    void exactMessageContextsExposeTheirContractFields() {
        Map<String, String> metadata = Map.of("tenant", "blue");
        var request = new DefaultRequestContext(
            "profile", "ProfileRequest", "application/json", metadata);
        assertEquals(Optional.empty(), request.meshName());
        assertEquals(Optional.of("profile"), request.channelName());
        assertEquals("ProfileRequest", request.packetName());
        assertEquals(Optional.of("application/json"), request.contentType());
        assertEquals(metadata, request.metadata());
        assertEquals(Optional.empty(), request.correlationId());

        var publish = new DefaultPublishContext(
            "events", "ProfileChanged", "profile.changed", "application/json", metadata);
        assertEquals("profile.changed", publish.topic());
        assertEquals(Optional.empty(), publish.source());

        RoutingId sourceNodeRid = RoutingId.from("source-node");
        var route = new DefaultRouteRequestContext(
            "mesh", "ProfileRequest", sourceNodeRid, "application/json", metadata);
        assertEquals(sourceNodeRid, route.sourceNodeRid());
    }

    @Test
    void copiedTransportPartsPreserveTheCallerPayloadForFallback() {
        try (Message payload = Message.from("payload")) {
            List<Message> copied = ZLinkChannelCallRuntime.copyParts(
                Optional.of("Packet"),
                payload,
                ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE);
            try {
                assertEquals("payload", payload.toUtf8String());
                assertEquals("payload", copied.get(1).toUtf8String());
            } finally {
                copied.forEach(Message::close);
            }
        }
    }

    @Test
    void channelRequestWithoutAnEgressRouteIsNotFound() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(),
            handlers())) {
            ZLinkFrameworkException failure = assertThrows(
                ZLinkFrameworkException.class,
                () -> runtime.requestToChannel("missing", new TestRequest("missing")));

            assertEquals(ZLinkFrameworkErrorKind.NOT_FOUND, failure.kind());
        }
    }

    @Test
    void channelSendUsesExplicitDeclaredTypeForCodecAndPacketName() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addClientServerChannel("typed")
            .client()
            .connect("inproc://typed");
        ZLinkCodecRegistration codecs = options.registration().codecs();
        codecs.addSerializer(
            "application/x-broad",
            new OutboundMarkerSerializer("BROAD"),
            type -> type == BaseOutbound.class || type == DerivedOutbound.class);
        codecs.addSerializer(
            "application/x-base",
            new OutboundMarkerSerializer("BASE"),
            BaseOutbound.class::equals);
        ZLinkMessageSerializer serializer = codecs.serializerWithFallback(
            new ZLinkJsonMessageSerializer());
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();

        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            serializer,
            handlers(),
            ZLinkTestAdmissionFactory.create())) {
            runtime.sendToChannel(
                    "typed",
                    ZLinkMessage.of(new DerivedOutbound(), BaseOutbound.class))
                .submit()
                .toCompletableFuture()
                .join();

            assertEquals("BaseOutbound", backend.dealer.lastSendParts.get(0).toUtf8String());
            assertEquals("BASE", backend.dealer.lastSendParts.get(1).toUtf8String());
            assertEquals(
                "application/x-base",
                ZLinkChannelContentTypeFrame.decode(backend.dealer.lastSendParts));
        }
    }

    @Test
    void serverOnlyClientServerRequestIsNotConfigured() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addClientServerChannel("api").server();
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(),
            handlers())) {
            // A registered ClientServer Channel that lacks the Client role is a
            // missing-role configuration (spec 32-framework-error-model
            // NotConfigured = "required role isn't registered"; 09-client-server
            // -channel: a Server can't start an outbound business call), not a
            // missing target (NotFound).
            ZLinkFrameworkException failure = assertThrows(
                ZLinkFrameworkException.class,
                () -> runtime.requestToChannel("api", new TestRequest("server-only")));

            assertEquals(ZLinkFrameworkErrorKind.NOT_CONFIGURED, failure.kind());
        }
    }

    @Test
    void serverOnlyClientServerSendIsNotConfigured() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addClientServerChannel("api").server();
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(),
            handlers())) {
            ZLinkFrameworkException failure = assertThrows(
                ZLinkFrameworkException.class,
                () -> runtime.sendToChannel("api", new TestRequest("server-only")));

            assertEquals(ZLinkFrameworkErrorKind.NOT_CONFIGURED, failure.kind());
        }
    }

    @Test
    void meshChannelRequestTerminatesOnBackendNotConnectedWithoutFrameworkRetry() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.spotNode.requestFailuresRemaining = 1;
        backend.spotNode.requestFailureResult = SubmitResult.NOT_CONNECTED;
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(),
            handlers())) {
            runtime.registerSpotRouterNode("play", backend.spotNode);

            CompletionException failure = assertThrows(
                CompletionException.class,
                () -> runtime.requestToChannel("play", new TestRequest("hello"))
                    .submit(TestReply.class).toCompletableFuture().join());

            ZlinkSubmitException terminal = assertInstanceOf(
                ZlinkSubmitException.class, failure.getCause());
            assertEquals(SubmitResult.NOT_CONNECTED, terminal.getResult());
            assertEquals(1, backend.spotNode.requestAttempts);
        }
    }

    @Test
    void meshChannelRequestReturnsTargetNotFoundBeforeTheRequestTimeout() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofSeconds(2));
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.spotNode.channelTargetClassification =
            Optional.of(ZLinkOneWayCalls.TARGET_NOT_FOUND);
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(),
            handlers())) {
            runtime.registerSpotRouterNode("play", backend.spotNode);

            ExecutionException error = Assertions.assertThrows(
                ExecutionException.class,
                () -> runtime.requestToChannel(
                        "play",
                        new TestRequest("no-target"))
                    .submit(TestReply.class)
                    .toCompletableFuture()
                    .get(1, TimeUnit.SECONDS));

            assertEquals(
                ZLinkFrameworkErrorKind.NOT_FOUND,
                ((ZLinkFrameworkException) error.getCause()).kind());
            assertEquals(0, backend.spotNode.requestAttempts);
        }
    }

    @Test
    void clientBuilderConnectsEveryConfiguredEndpoint() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addClientServerChannel("work")
            .client()
            .connect("inproc://first")
            .connect("inproc://second");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();

        try (ZLinkChannelRuntime ignored = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            assertEquals(List.of("inproc://first", "inproc://second"), backend.dealer.connected);
        }
    }

    @Test
    void methodArgumentsBindMessageAndContextOnly() throws Exception {
        FakeRequestContext context = new FakeRequestContext();
        Method method = ContextHandler.class.getMethod(
            "handle",
            String.class,
            ZLinkMessageContext.class);

        Object[] arguments = ZLinkChannelHandlerInvoker.methodArguments(method, "hello", context);

        assertSame("hello", arguments[0]);
        assertSame(context, arguments[1]);
        assertEquals(2, arguments.length);
    }

    @Test
    void routeBridgeAsyncRequestCompletesFromCanonicalBackendTerminal() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.bridge.requestReplyParts = List.of(Message.from(
            "{\"ok\":true,\"response\":{\"value\":\"reply\"}}".getBytes()));
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            runtime.registerSpotRouteBridgeOwner(() -> backend.spotNode);
            var request = runtime.requestToSpotViaRouterChannel(
                "play.route",
                RoutingId.from("play-node"),
                "room-spot",
                List.of(Message.from("raw-request".getBytes())),
                Duration.ofMillis(300));

            List<Message> reply = request.toCompletableFuture().get(1, TimeUnit.SECONDS);
            try {
                assertEquals(1, reply.size());
                assertEquals("{\"ok\":true,\"response\":{\"value\":\"reply\"}}", reply.get(0).toUtf8String());
            } finally {
                reply.forEach(Message::close);
            }
            assertFalse(backend.bridge.nativeCallbackInvoked);
        }
    }

    @Test
    void routeClientSpotRequestUsesRouteBridge() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.bridge.requestReplyParts = List.of(Message.from(
            "{\"ok\":true,\"response\":{\"value\":\"reply\"}}".getBytes()));
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            runtime.registerSpotRouteBridgeOwner(() -> backend.spotNode);

            TestReply reply = runtime.requestToSpot(
                    "room-spot",
                    new TestRequest("hello"))
                .timeout(Duration.ofMillis(300))
                .submit(TestReply.class).toCompletableFuture().join();

            assertEquals("reply", reply.value());
            assertEquals("play.route", backend.bridge.lastChannelName);
            assertEquals(RoutingId.from("play-node"), backend.bridge.lastTargetNodeRid);
            assertEquals("room-spot", backend.bridge.lastTargetSpotId);
            assertEquals(
                "TestRequest",
                ZLinkChannelEnvelope.decodeHeader(
                    backend.bridge.lastParts.get(0), false).messageName());
        }
    }

    @Test
    void routeClientSpotSendUsesRouteBridge() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            runtime.registerSpotRouteBridgeOwner(() -> backend.spotNode);

            runtime.sendToSpot(
                    "room-spot",
                    new TestRequest("hello"))
                .submit().toCompletableFuture().join();

            assertEquals("play.route", backend.bridge.lastChannelName);
            assertEquals(RoutingId.from("play-node"), backend.bridge.lastTargetNodeRid);
            assertEquals("room-spot", backend.bridge.lastTargetSpotId);
            assertEquals(
                "TestRequest",
                ZLinkChannelEnvelope.decodeHeader(
                    backend.bridge.lastParts.get(0), false).messageName());
        }
    }

    @Test
    void routedSpotSendRegistersLifecycleBeforeFirstAttemptAndClosesPayloads() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofSeconds(1));
        ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.bridge.acceptSends = false;
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers());
            Message payload = Message.from("payload")) {
            runtime.registerSpotRouteBridgeOwner(() -> backend.spotNode);
            backend.bridge.onSend = runtime::beginClose;

            CompletableFuture<Void> send = runtime.sendToSpotViaRouterChannel(
                    "play.route",
                    RoutingId.from("play-node"),
                    "room-spot",
                    List.of(payload))
                .toCompletableFuture();

            CompletionException completion = assertThrows(
                CompletionException.class,
                send::join);
            ZLinkFrameworkException failure = assertInstanceOf(
                ZLinkFrameworkException.class,
                completion.getCause());
            assertEquals(ZLinkFrameworkErrorKind.SHUTTING_DOWN, failure.kind());
            assertEquals("channel runtime is closed", failure.getMessage());
            assertEquals(1, backend.bridge.sendAttempts);
            assertTrue(backend.bridge.lastAttemptParts.stream().allMatch(Message::empty));
        }
    }

    @Test
    void routeReceiveLoopSkipsRouterProbeFrame() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            runtime.registerSpotRouteBridgeOwner(() -> backend.spotNode);

            backend.router.inbound.add(new ZLinkBackendReceived(
                Optional.of(RoutingId.from("route-a-peer")),
                Optional.empty(),
                Optional.empty(),
                List.of(Message.from(new byte[0]))));

            Thread.sleep(50);
            assertEquals(0, backend.router.replyCount);
            assertFalse(backend.bridge.routerReceivedInvoked);
        }
    }

    @Test
    void routeMeshDoesNotApplyClientServerMessageSizeOption() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        ZLinkLegacyTopology
            .addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();

        try (ZLinkChannelRuntime ignored = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(),
            handlers())) {
            assertEquals(0, backend.router.maxMessageSize());
            assertEquals(100, backend.router.peerWeight);
        }
    }

    @Test
    void channelRequestTerminatesOnBackendNotConnectedWithoutFrameworkRetry() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        options.addClientServerChannel("profile")
            .client()
            .connect("inproc://profile");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.dealer.requestFailuresRemaining = 1;
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            CompletionException failure = assertThrows(
                CompletionException.class,
                () -> runtime.requestToChannel("profile", new TestRequest("hello"))
                    .timeout(Duration.ofMillis(300))
                    .submit(TestReply.class).toCompletableFuture().join());

            ZlinkSubmitException terminal = assertInstanceOf(
                ZlinkSubmitException.class, failure.getCause());
            assertEquals(SubmitResult.NOT_CONNECTED, terminal.getResult());
            assertEquals(1, backend.dealer.requestAttempts);
        }
    }

    @Test
    void routeRequestTerminatesOnBackendNotConnectedWithoutFrameworkRetry() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route")
            .enableClient("inproc://play-peer");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.router.requestFailuresRemaining = 1;
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            CompletionException failure = assertThrows(
                CompletionException.class,
                () -> runtime.requestToNode(
                        "play.route",
                        RoutingId.from("play-node"),
                        new TestRequest("hello"))
                    .timeout(Duration.ofMillis(300))
                    .submit(TestReply.class).toCompletableFuture().join());

            ZlinkSubmitException terminal = assertInstanceOf(
                ZlinkSubmitException.class, failure.getCause());
            assertEquals(SubmitResult.NOT_CONNECTED, terminal.getResult());
            assertEquals(1, backend.router.requestAttempts);
        }
    }

    @Test
    void meshNodeAndChannelCallsEncodeImmutableApplicationMetadata() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
                 backend,
                 options.registration(),
                 new ZLinkJsonMessageSerializer(),
                 handlers(),
                 systems.zlink.framework.runtime.host
                     .ZLinkTestAdmissionFactory.create())) {
            runtime.registerSpotRouterNode("mesh", backend.spotNode);
            runtime.registerSpotRouterNode("play", backend.spotNode);

            runtime.sendToNode(
                    "mesh",
                    RoutingId.from("peer"),
                    new TestRequest("node"))
                .metadata("trace-id", "node")
                .submit();
            backend.spotNode.metadataObserved
                .orTimeout(1, TimeUnit.SECONDS)
                .join();
            assertEquals(
                Map.of("trace-id", "node"),
                ZLinkApplicationMetadata.decode(
                    backend.spotNode.lastMetadata));

            Map<String, String> source =
                new LinkedHashMap<>(Map.of("tenant", "blue"));
            TestReply reply = runtime.requestToChannel(
                    "play",
                    new TestRequest("channel"))
                .metadata(source)
                .metadata("tenant", "green")
                .submit(TestReply.class)
                .toCompletableFuture()
                .join();
            source.put("tenant", "mutated");

            assertEquals("reply", reply.value());
            assertEquals(
                Map.of("tenant", "green"),
                ZLinkApplicationMetadata.decode(
                    backend.spotNode.lastMetadata));
        }
    }

    @Test
    void immutableMeshNodeSendOptionsShareTheSingleUseTerminal() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
                 backend,
                 options.registration(),
                 new ZLinkJsonMessageSerializer(),
                 handlers(),
                 systems.zlink.framework.runtime.host
                     .ZLinkTestAdmissionFactory.create())) {
            runtime.registerSpotRouterNode("mesh", backend.spotNode);

            var base = runtime.sendToNode(
                "mesh", RoutingId.from("peer"), new TestRequest("once"));
            var configured = base.metadata("trace-id", "once");
            configured.submit().toCompletableFuture().join();

            CompletionException duplicate = assertThrows(
                CompletionException.class,
                () -> base.submit().toCompletableFuture().join());
            assertEquals(
                ZLinkFrameworkErrorKind.INVALID_OPERATION,
                ((ZLinkFrameworkException) duplicate.getCause()).kind());
        }
    }

    @Test
    void localNodeSendTerminatesBackpressureWithoutFrameworkRetry() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.spotNode.localNodeStatuses.add(1);
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
                 backend,
                 options.registration(),
                 new ZLinkJsonMessageSerializer(),
                 handlers(),
                 systems.zlink.framework.runtime.host
                     .ZLinkTestAdmissionFactory.create())) {
            runtime.registerSpotRouterNode("mesh", backend.spotNode);

            var result = runtime.sendToNode(
                    "mesh",
                    RoutingId.from("owner-node"),
                    new TestRequest("local"))
                .submit()
                .toCompletableFuture();

            assertTrue(result.isDone());
            assertEquals(2, OneWayTestStatus.status(result));
            assertEquals(1, backend.spotNode.localNodeAttempts);
            backend.spotNode.signalLocalNodeReady();
            assertEquals(1, backend.spotNode.localNodeAttempts);
        }
    }

    @Test
    void spotRouterNodeRequestTerminatesOnBackendNotConnectedWithoutFrameworkRetry() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.spotNode.entrySpot.requestFailuresRemaining = 1;
        backend.spotNode.entrySpot.requestFailureResult = SubmitResult.NOT_CONNECTED;
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            runtime.registerSpotRouterNode("play.route", backend.spotNode);

            CompletionException failure = assertThrows(
                CompletionException.class,
                () -> runtime.requestToSpot(
                        "room-spot",
                        new TestRequest("hello"))
                    .timeout(Duration.ofMillis(300))
                    .submit(TestReply.class).toCompletableFuture().join());

            ZlinkSubmitException terminal = assertInstanceOf(
                ZlinkSubmitException.class, failure.getCause());
            assertEquals(SubmitResult.NOT_CONNECTED, terminal.getResult());
            assertEquals(1, backend.spotNode.entrySpot.requestAttempts);
            assertEquals(SendFlags.NONE, backend.spotNode.entrySpot.lastRequestFlags);
            assertEquals(1L, backend.spotNode.entrySpot.lastSpotGeneration);
        }
    }

    @Test
    void spotRouterNodeCallsInstallTheResolvedAuthorityBeforeSubmit() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.spotNode.entrySpot.requestReplyParts = List.of(
            Message.from("{\"value\":\"reply\"}".getBytes()));
        RoutingId owner = RoutingId.from("play-node");
        SpotTransportAddressResolver resolver = spotId ->
            CompletableFuture.completedFuture(Optional.of(
                new SpotTransportAddress(
                    "play.route",
                    owner,
                    spotId,
                    17L,
                    23L,
                    29L,
                    31L,
                    ZLinkSpotKind.USER)));
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(),
            handlers(resolver))) {
            runtime.registerSpotRouterNode("play.route", backend.spotNode);

            runtime.sendToSpot("room-spot", new TestRequest("send"))
                .submit()
                .toCompletableFuture()
                .join();
            runtime.requestToSpot("room-spot", new TestRequest("request"))
                .submit(TestReply.class)
                .toCompletableFuture()
                .join();

            assertEquals(2, backend.spotNode.entrySpot.authorityRemembers);
            assertEquals(owner, backend.spotNode.entrySpot.authorityNodeRid);
            assertEquals("room-spot", backend.spotNode.entrySpot.authoritySpotId);
            assertEquals(17L, backend.spotNode.entrySpot.authoritySpotGeneration);
            assertEquals(29L, backend.spotNode.entrySpot.authorityOwnerGeneration);
            assertEquals(31L, backend.spotNode.entrySpot.authorityOwnerLeaseGeneration);
            assertEquals(1, backend.spotNode.entrySpot.sendAttempts);
            assertEquals(1, backend.spotNode.entrySpot.requestAttempts);
        }
    }

    @Test
    void spotRouterNodeRequestCompletesUnavailableOnTransportNotConnectedReply() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.spotNode.entrySpot.requestResults.add(
            ZLinkBackendRequestResult.NOT_CONNECTED);
        backend.spotNode.entrySpot.requestReplyParts = List.of(
            Message.from("{\"value\":\"reply\"}".getBytes()));
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            runtime.registerSpotRouterNode("play.route", backend.spotNode);

            var failure = Assertions.assertThrows(
                CompletionException.class,
                () -> runtime.requestToSpot(
                    "room-spot",
                    new TestRequest("hello"))
                .timeout(Duration.ofMillis(300))
                .submit(TestReply.class)
                .toCompletableFuture()
                .join());

            var frameworkError = assertInstanceOf(
                ZLinkFrameworkException.class,
                failure.getCause());
            assertEquals(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                frameworkError.kind());
            assertEquals(1, backend.spotNode.entrySpot.requestAttempts);
        }
    }

    @Test
    void spotRouterNodeSendTerminatesOnBackendNotConnectedWithoutFrameworkRetry() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.spotNode.entrySpot.sendFailuresRemaining = 1;
        backend.spotNode.entrySpot.sendFailureResult = SubmitResult.NOT_CONNECTED;
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            runtime.registerSpotRouterNode("play.route", backend.spotNode);

            CompletionException failure = assertThrows(
                CompletionException.class,
                () -> runtime.sendToSpot(
                        "room-spot",
                        new TestRequest("hello"))
                    .submit().toCompletableFuture().join());

            ZlinkSubmitException terminal = assertInstanceOf(
                ZlinkSubmitException.class, failure.getCause());
            assertEquals(SubmitResult.NOT_CONNECTED, terminal.getResult());
            assertEquals(1, backend.spotNode.entrySpot.sendAttempts);
            assertEquals(SendFlags.NONE, backend.spotNode.entrySpot.lastSendFlags);
            assertEquals(1L, backend.spotNode.entrySpot.lastSpotGeneration);
        }
    }

    @Test
    void spotRouterNodeRequestFailsOnBackendRequestResultWithoutTreatingEmptyReplyAsSuccess() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.spotNode.entrySpot.requestResult = ZLinkBackendRequestResult.NOT_FOUND;
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            runtime.registerSpotRouterNode("play.route", backend.spotNode);

            CompletionException error = Assertions.assertThrows(
                CompletionException.class,
                () -> runtime.requestToSpot(
                        "room-spot",
                        new TestRequest("hello"))
                    .timeout(Duration.ofMillis(300))
                    .submit(TestReply.class).toCompletableFuture().join());

            assertInstanceOf(ZLinkFrameworkException.class, error.getCause());
            assertTrue(error.getCause().getMessage().contains("NOT_FOUND"));
            assertEquals(1L, backend.spotNode.entrySpot.lastSpotGeneration);
        }
    }

    @Test
    void staleSpotReplyRefreshesAuthorityBeforeRetryingTheRequest() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.spotNode.entrySpot.requestResults.add(
            ZLinkBackendRequestResult.NOT_FOUND);
        backend.spotNode.entrySpot.requestResults.add(
            ZLinkBackendRequestResult.OK);
        backend.spotNode.entrySpot.requestReplyParts = List.of(
            Message.from("{\"value\":\"fresh\"}".getBytes()));
        AtomicInteger resolves = new AtomicInteger();
        AtomicInteger invalidations = new AtomicInteger();
        SpotTransportAddressResolver resolver = new SpotTransportAddressResolver() {
            @Override
            public CompletionStage<Optional<SpotTransportAddress>>
                resolve(String spotId) {
                long generation = resolves.incrementAndGet();
                return CompletableFuture.completedFuture(Optional.of(
                    new SpotTransportAddress(
                        "play.route",
                        RoutingId.from("play-node"),
                        spotId,
                        generation,
                        ZLinkSpotKind.USER)));
            }

            @Override
            public void invalidate(String spotId) {
                invalidations.incrementAndGet();
            }
        };
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(),
            handlers(resolver))) {
            runtime.registerSpotRouterNode("play.route", backend.spotNode);

            TestReply reply = runtime.requestToSpot(
                    "room-spot",
                    new TestRequest("stale"))
                .timeout(Duration.ofMillis(300))
                .submit(TestReply.class)
                .toCompletableFuture()
                .join();

            assertEquals("fresh", reply.value());
            assertEquals(2, resolves.get());
            assertEquals(1, invalidations.get());
            assertEquals(2L, backend.spotNode.entrySpot.lastSpotGeneration);
        }
    }

    @Test
    void instanceSpotRequestDoesNotActivateWhenReadyOwnerIsUnavailable() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        AtomicInteger activationAttempts = new AtomicInteger();
        SpotTransportAddressResolver resolver = spotId ->
            CompletableFuture.failedFuture(new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                "ready owner lease expired"));
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers(resolver))) {
            runtime.registerSpotRouterNode("play.route", backend.spotNode);
            runtime.registerInstanceSpotCallRuntime(new ZLinkInstanceSpotCallRuntime() {
                @Override
                public CompletionStage<Void> send(
                    String spotId,
                    String stableType,
                    String meshName,
                    Message payload,
                    Optional<String> packetName,
                    String contentType,
                    Map<String, String> metadata) {
                    return CompletableFuture.completedFuture(null);
                }

                @Override
                public CompletionStage<List<Message>> request(
                    String spotId,
                    String stableType,
                    String meshName,
                    Message payload,
                    Optional<String> packetName,
                    String contentType,
                    Map<String, String> metadata,
                    Duration timeout) {
                    activationAttempts.incrementAndGet();
                    byte[] reply = new ZLinkJsonMessageSerializer()
                        .serialize(new TestReply("reactivated"))
                        .bytes();
                    return CompletableFuture.completedFuture(List.of(Message.from(reply)));
                }
            });

            CompletionException failure = assertThrows(
                CompletionException.class,
                () -> runtime.requestToSpot(
                        "room-spot",
                        new TestRequest("hello"))
                    .instanceSpot("room")
                    .inMesh("play.route")
                    .submit(TestReply.class)
                    .toCompletableFuture()
                    .join());

            assertEquals(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            assertEquals(0, activationAttempts.get());
            assertEquals(0, backend.spotNode.entrySpot.requestAttempts);
        }
    }

    @Test
    void routeBridgeAsyncRequestCompletesWithoutRouterRequestSequence() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.bridge.requestReplyParts = List.of(Message.from(
            "{\"ok\":true,\"response\":{\"value\":\"reply\"}}".getBytes()));
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            runtime.registerSpotRouteBridgeOwner(() -> backend.spotNode);
            var request = runtime.requestToSpotViaRouterChannel(
                "play.route",
                RoutingId.from("play-node"),
                "room-spot",
                List.of(Message.from("raw-request".getBytes())),
                Duration.ofMillis(300));

            List<Message> reply = request.toCompletableFuture().get(1, TimeUnit.SECONDS);
            try {
                assertEquals(1, reply.size());
                assertEquals("{\"ok\":true,\"response\":{\"value\":\"reply\"}}", reply.get(0).toUtf8String());
            } finally {
                reply.forEach(Message::close);
            }
            assertFalse(backend.bridge.routerReceivedInvoked);
            assertEquals(0, backend.router.replyCount);
        }
    }

    @Test
    void routeBridgeAsyncRequestUnwrapsRoutedSpotEnvelopeReply() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.bridge.requestReplyParts = List.of(
            Message.from("__zlink.routed_spot.egress.request".getBytes()),
            Message.from("StateReply".getBytes()),
            Message.from("{\"spotId\":\"room-spot\",\"value\":\"pong\"}".getBytes()));
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            runtime.registerSpotRouteBridgeOwner(() -> backend.spotNode);
            var request = runtime.requestToSpotViaRouterChannel(
                "play.route",
                RoutingId.from("play-node"),
                "room-spot",
                List.of(
                    Message.from("StateRequest".getBytes()),
                    Message.from("{\"op\":\"ping\"}".getBytes())),
                Duration.ofMillis(300));

            List<Message> reply = request.toCompletableFuture().get(1, TimeUnit.SECONDS);
            try {
                assertEquals(1, reply.size());
                assertEquals("{\"spotId\":\"room-spot\",\"value\":\"pong\"}", reply.get(0).toUtf8String());
            } finally {
                reply.forEach(Message::close);
            }
            assertFalse(backend.bridge.routerReceivedInvoked);
            assertEquals(0, backend.router.replyCount);
        }
    }

    @Test
    void routeBridgeNativeCallbackRequestUnwrapsRoutedSpotEnvelopeReply() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.bridge.requestReplyParts = List.of(
            Message.from("__zlink.routed_spot.egress.request".getBytes()),
            Message.from("StateReply".getBytes()),
            Message.from("{\"spotId\":\"room-spot\",\"value\":\"pong\"}".getBytes()));
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            runtime.registerSpotRouteBridgeOwner(() -> backend.spotNode);
            var request = runtime.requestToSpotViaRouterChannel(
                "play.route",
                RoutingId.from("play-node"),
                "room-spot",
                List.of(
                    Message.from("StateRequest".getBytes()),
                    Message.from("{\"op\":\"ping\"}".getBytes())),
                Duration.ofMillis(300));

            List<Message> reply = request.toCompletableFuture().get(1, TimeUnit.SECONDS);
            try {
                assertEquals(1, reply.size());
                assertEquals("{\"spotId\":\"room-spot\",\"value\":\"pong\"}", reply.get(0).toUtf8String());
            } finally {
                reply.forEach(Message::close);
            }
        }
    }

    @Test
    void routeBridgeNativeCallbackRequestStripsSpotPacketNameReply() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.bridge.requestReplyParts = List.of(
            Message.from("StateReply".getBytes()),
            Message.from("{\"spotId\":\"room-spot\",\"value\":\"pong\"}".getBytes()));
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            runtime.registerSpotRouteBridgeOwner(() -> backend.spotNode);
            var request = runtime.requestToSpotViaRouterChannel(
                "play.route",
                RoutingId.from("play-node"),
                "room-spot",
                List.of(
                    Message.from("StateRequest".getBytes()),
                    Message.from("{\"op\":\"ping\"}".getBytes())),
                Duration.ofMillis(300));

            List<Message> reply = request.toCompletableFuture().get(1, TimeUnit.SECONDS);
            try {
                assertEquals(1, reply.size());
                assertEquals("{\"spotId\":\"room-spot\",\"value\":\"pong\"}", reply.get(0).toUtf8String());
            } finally {
                reply.forEach(Message::close);
            }
        }
    }

    @Test
    void routeBridgeAsyncRequestUsesCanonicalBackendReplyRatherThanEcho() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.bridge.requestReplyParts = List.of(
            Message.from("__zlink.routed_spot.egress.request".getBytes()),
            Message.from("StateReply".getBytes()),
            Message.from("{\"spotId\":\"room-spot\",\"value\":\"pong\"}".getBytes()));
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            runtime.registerSpotRouteBridgeOwner(() -> backend.spotNode);
            var request = runtime.requestToSpotViaRouterChannel(
                "play.route",
                RoutingId.from("play-node"),
                "room-spot",
                List.of(
                    Message.from("StateRequest".getBytes()),
                    Message.from("{\"op\":\"ping\"}".getBytes())),
                Duration.ofMillis(300));

            List<Message> reply = request.toCompletableFuture().get(1, TimeUnit.SECONDS);
            try {
                assertEquals(1, reply.size());
                assertEquals("{\"spotId\":\"room-spot\",\"value\":\"pong\"}", reply.get(0).toUtf8String());
            } finally {
                reply.forEach(Message::close);
            }
        }
    }

    @Test
    void routeBridgeAsyncRequestCompletesFromBackendTerminalAfterRouterEcho() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.bridge.completeRequests = false;
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            runtime.registerSpotRouteBridgeOwner(() -> backend.spotNode);
            var request = runtime.requestToSpotViaRouterChannel(
                "play.route",
                RoutingId.from("play-node"),
                "room-spot",
                List.of(
                    Message.from("StateRequest".getBytes()),
                    Message.from("{\"op\":\"ping\"}".getBytes())),
                Duration.ofMillis(300));

            backend.router.inbound.add(new ZLinkBackendReceived(
                Optional.of(RoutingId.from("play-node")),
                Optional.empty(),
                Optional.of(6L),
                List.of(
                    Message.from("__zlink.routed_spot.egress.request".getBytes()),
                    Message.from("StateRequest".getBytes()))));
            backend.router.inbound.add(new ZLinkBackendReceived(
                Optional.of(RoutingId.from("play-node")),
                Optional.empty(),
                Optional.of(7L),
                List.of(
                    Message.from("__zlink.routed_spot.egress.request".getBytes()),
                    Message.from("StateReply".getBytes()),
                    Message.from("{\"spotId\":\"room-spot\",\"value\":\"pong\"}".getBytes()))));
            backend.bridge.completePendingRequest(List.of(
                Message.from("__zlink.routed_spot.egress.request".getBytes()),
                Message.from("StateReply".getBytes()),
                Message.from("{\"spotId\":\"room-spot\",\"value\":\"pong\"}".getBytes())));

            List<Message> reply = request.toCompletableFuture().get(1, TimeUnit.SECONDS);
            try {
                assertEquals(1, reply.size());
                assertEquals("{\"spotId\":\"room-spot\",\"value\":\"pong\"}", reply.get(0).toUtf8String());
            } finally {
                reply.forEach(Message::close);
            }
        }
    }

    @Test
    void routeBridgeLateRequestAfterNativeCompletionIsRepliedAsMissingRouteHandler()
        throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            runtime.registerSpotRouteBridgeOwner(() -> backend.spotNode);
            runtime.requestToSpotViaRouterChannel(
                    "play.route",
                    RoutingId.from("play-node"),
                    "room-spot",
                    List.of(Message.from("raw-request".getBytes())),
                    Duration.ofMillis(300))
                .toCompletableFuture()
                .get(1, TimeUnit.SECONDS);

            backend.router.inbound.add(new ZLinkBackendReceived(
                Optional.of(RoutingId.from("play-node")),
                Optional.empty(),
                Optional.of(7L),
                List.of(
                    Message.from("StateRequest".getBytes()),
                    Message.from("{\"op\":\"late\"}".getBytes()))));
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(1);
            while (!backend.router.inbound.isEmpty() && System.nanoTime() < deadline) {
                Thread.sleep(10);
            }

            assertTrue(backend.router.inbound.isEmpty());
            Thread.sleep(50);
            assertEquals(1, backend.router.replyCount);
        }
    }

    @Test
    void routeBridgeAsyncRequestCompletesWithoutBridgeFeedback() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.bridge.requestReplyParts = List.of(
            Message.from("true\nactor-node\nplayer-x\n1\ncmVwbHk=".getBytes()));
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            runtime.registerSpotRouteBridgeOwner(() -> backend.spotNode);
            var request = runtime.requestToSpotViaRouterChannel(
                "play.route",
                RoutingId.from("play-node"),
                "room-spot",
                List.of(Message.from("raw-request".getBytes())),
                Duration.ofMillis(300));

            List<Message> reply = request.toCompletableFuture().get(1, TimeUnit.SECONDS);
            try {
                assertEquals(1, reply.size());
                assertEquals("true\nactor-node\nplayer-x\n1\ncmVwbHk=", reply.get(0).toUtf8String());
            } finally {
                reply.forEach(Message::close);
            }
            assertFalse(backend.bridge.routerReceivedInvoked);
        }
    }

    @Test
    void routeBridgeAsyncRequestFailsOnCanonicalFrameworkErrorReply() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.bridge.requestReplyParts = ZLinkFrameworkErrorReply.create(
            systems.zlink.framework.errors.ZLinkFrameworkErrorKind.NOT_FOUND,
            "missing route handler");
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            runtime.registerSpotRouteBridgeOwner(() -> backend.spotNode);
            var request = runtime.requestToSpotViaRouterChannel(
                "play.route",
                RoutingId.from("play-node"),
                "room-spot",
                List.of(Message.from("raw-request".getBytes())),
                Duration.ofMillis(300));

            ExecutionException error = Assertions.assertThrows(
                ExecutionException.class,
                () -> request.toCompletableFuture().get(1, TimeUnit.SECONDS));
            assertInstanceOf(ZLinkFrameworkException.class, error.getCause());
            assertEquals("missing route handler", error.getCause().getMessage());
            assertFalse(backend.bridge.routerReceivedInvoked);
        }
    }

    @Test
    void routeMeshFrameworkErrorRequestIsDroppedWithoutErrorReply() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            backend.router.inbound.add(new ZLinkBackendReceived(
                Optional.of(RoutingId.from("play-node")),
                Optional.empty(),
                Optional.of(7L),
                List.of(
                    Message.from("ZLinkFrameworkError".getBytes()),
                    Message.from("missing route handler".getBytes()))));

            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(1);
            while (!backend.router.inbound.isEmpty() && System.nanoTime() < deadline) {
                Thread.sleep(10);
            }

            assertTrue(backend.router.inbound.isEmpty());
            Thread.sleep(50);
            assertEquals(0, backend.router.replyCount);
            assertFalse(backend.bridge.routerReceivedInvoked);
        }
    }

    @Test
    void routeBridgeDrainFailureDoesNotStopLaterAsyncTerminalCompletion() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.bridge.completeRequests = false;
        backend.bridge.drainFailuresRemaining = 1;
        backend.bridge.firstDrainFailed = new CountDownLatch(1);
        backend.bridge.nextDrainAfterFailure = new CountDownLatch(1);
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            runtime.registerSpotRouteBridgeOwner(() -> backend.spotNode);

            var request = runtime.requestToSpotViaRouterChannel(
                "play.route",
                RoutingId.from("play-node"),
                "room-spot",
                List.of(Message.from("raw-request".getBytes())),
                Duration.ofMillis(300));
            assertTrue(backend.bridge.firstDrainFailed.await(1, TimeUnit.SECONDS));

            backend.router.inbound.add(new ZLinkBackendReceived(
                Optional.of(RoutingId.from("play-node")),
                Optional.empty(),
                Optional.empty(),
                List.of(Message.from("{\"ok\":true}".getBytes()))));
            assertTrue(backend.bridge.nextDrainAfterFailure.await(1, TimeUnit.SECONDS));
            backend.bridge.completePendingRequest(List.of(
                Message.from("{\"ok\":true}".getBytes())));

            List<Message> reply = request.toCompletableFuture().get(1, TimeUnit.SECONDS);
            try {
                assertEquals(1, reply.size());
                assertEquals("{\"ok\":true}", reply.get(0).toUtf8String());
            } finally {
                reply.forEach(Message::close);
            }
            assertEquals(0, backend.bridge.drainFailuresRemaining);
        }
    }

    @Test
    void routeBridgeNoDataDrainDoesNotStopLaterAsyncTerminalCompletion() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.bridge.completeRequests = false;
        backend.bridge.noDataDrainsRemaining = 1;
        backend.bridge.firstDrainFailed = new CountDownLatch(1);
        backend.bridge.nextDrainAfterFailure = new CountDownLatch(1);
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            runtime.registerSpotRouteBridgeOwner(() -> backend.spotNode);

            var request = runtime.requestToSpotViaRouterChannel(
                "play.route",
                RoutingId.from("play-node"),
                "room-spot",
                List.of(Message.from("raw-request".getBytes())),
                Duration.ofMillis(300));
            assertTrue(backend.bridge.firstDrainFailed.await(1, TimeUnit.SECONDS));

            backend.router.inbound.add(new ZLinkBackendReceived(
                Optional.of(RoutingId.from("play-node")),
                Optional.empty(),
                Optional.empty(),
                List.of(Message.from("{\"ok\":true}".getBytes()))));
            assertTrue(backend.bridge.nextDrainAfterFailure.await(1, TimeUnit.SECONDS));
            backend.bridge.completePendingRequest(List.of(
                Message.from("{\"ok\":true}".getBytes())));

            List<Message> reply = request.toCompletableFuture().get(1, TimeUnit.SECONDS);
            try {
                assertEquals(1, reply.size());
                assertEquals("{\"ok\":true}", reply.get(0).toUtf8String());
            } finally {
                reply.forEach(Message::close);
            }
            assertEquals(0, backend.bridge.noDataDrainsRemaining);
        }
    }

    @Test
    void clientServerRequestCompletesExceptionallyWhenBackendResultIsNotOk() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        options.addClientServerChannel("api")
            .client()
            .connect("inproc://api");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.dealer.requestResult = ZLinkBackendRequestResult.TIMED_OUT;
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            var request = runtime.requestToChannel("api", "payload")
                .timeout(Duration.ofMillis(300))
                .submit(String.class);

            ExecutionException error = Assertions.assertThrows(
                ExecutionException.class,
                () -> request.toCompletableFuture().get(1, TimeUnit.SECONDS));
            ZLinkFrameworkException failure = assertInstanceOf(
                ZLinkFrameworkException.class, error.getCause());
            assertEquals(
                ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED, failure.kind());
        }
    }

    @Test
    void clientServerRuntimeOptionsReadAndWriteServerWeight() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addClientServerChannel("api")
            .server()
            .listen();
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            var socket = runtime.clientServerChannel("api").configureServerSocket();

            assertEquals(100, socket.weight());
            socket.weight(0);
            assertEquals(0, socket.weight());
            socket.weight(10_000);
            assertEquals(10_000, socket.weight());
        }
    }

    @Test
    void processLocalClientServerUsesManagedAdmissionAndDescriptorUpdates() {
        String endpoint = "tcp://127.0.0.1:40501";
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addClientServerChannel("orders").client();
        options.addClientServerChannel("orders").server()
            .setBindHost("127.0.0.1")
            .listen(40501);
        ManagedLocalBackend backend = new ManagedLocalBackend(endpoint, 100);
        ManagedLocalProvider provider = new ManagedLocalProvider(backend);

        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            provider,
            new ZLinkBackendAdapterOptions(Duration.ofSeconds(1)),
            options.registration(),
            new ZLinkJsonMessageSerializer(),
            handlers())) {
            ManagedAdmissionDealer local = backend.admissionDealer();

            assertEquals(List.of(endpoint), local.connected);
            assertEquals(1, local.admissionRequests);
            assertTrue(canSelect(runtime, "orders"));

            local.enqueueUpdate(local.descriptorWith(
                2, 0, ZLinkFrameworkRuntimeState.SERVING));
            awaitSelection(runtime, "orders", false);

            local.enqueueUpdate(local.descriptorWith(
                3, 100, ZLinkFrameworkRuntimeState.SERVING));
            awaitSelection(runtime, "orders", true);

            local.enqueueUpdate(local.descriptorWith(
                4, 100, ZLinkFrameworkRuntimeState.DRAINING));
            awaitSelection(runtime, "orders", false);
        }
    }

    private static boolean canSelect(
        ZLinkChannelRuntime runtime,
        String channelName) {
        try {
            runtime.requestToChannel(channelName, "probe");
            return true;
        } catch (ZLinkFrameworkException failure) {
            // 05-framework-api.ko.md 짠13: an empty selectable-target snapshot on a registered
            // send route is RequestTargetNotFound, not RouteNotConnected.
            assertEquals(
                ZLinkFrameworkErrorKind.NOT_FOUND, failure.kind());
            return false;
        }
    }

    private static void awaitSelection(
        ZLinkChannelRuntime runtime,
        String channelName,
        boolean expected) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
        do {
            if (canSelect(runtime, channelName) == expected) {
                return;
            }
            try {
                Thread.sleep(10);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                throw new AssertionError(interrupted);
            }
        } while (System.nanoTime() < deadline);
        assertEquals(expected, canSelect(runtime, channelName));
    }

    @Test
    void clientServerRuntimeOptionsReadAndWriteServerMaxMessageSize() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addClientServerChannel("api")
            .server()
            .listen();
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            var socket = runtime.clientServerChannel("api").configureServerSocket();

            assertEquals(16_777_216L, socket.maxMessageSize());
            socket.maxMessageSize(2 * 1024 * 1024L);
            assertEquals(2 * 1024 * 1024L, socket.maxMessageSize());
        }
    }

    @Test
    void clientServerRuntimeOptionsRejectNegativeMaxMessageSize() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addClientServerChannel("api")
            .server()
            .listen();
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            var socket = runtime.clientServerChannel("api").configureServerSocket();

            Assertions.assertThrows(
                ZLinkConfigurationException.class,
                () -> socket.maxMessageSize(-1));
        }
    }

    @Test
    void clientServerRuntimeOptionsRejectInvalidServerWeight() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addClientServerChannel("api")
            .server()
            .listen();
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            var socket = runtime.clientServerChannel("api").configureServerSocket();

            Assertions.assertThrows(
                ZLinkConfigurationException.class,
                () -> socket.weight(-1));
            Assertions.assertThrows(
                ZLinkConfigurationException.class,
                () -> socket.weight(10_001));
        }
    }

    public static final class ContextHandler {
        public void handle(
            String request,
            ZLinkMessageContext context) {
        }
    }

    private static final class FakeRequestContext implements ZLinkMessageContext {
        @Override
        public Optional<String> meshName() {
            return Optional.empty();
        }

        @Override
        public Optional<String> channelName() {
            return Optional.of("profile");
        }

        @Override
        public String packetName() {
            return "Echo";
        }

        @Override
        public Optional<String> contentType() {
            return Optional.empty();
        }

        @Override
        public Map<String, String> metadata() {
            return Map.of();
        }

        @Override
        public Optional<String> correlationId() {
            return Optional.empty();
        }
    }

    private static final class ManagedLocalProvider
        implements ZLinkBackendAdapterProvider {
        private final ManagedLocalBackend backend;

        private ManagedLocalProvider(ManagedLocalBackend backend) {
            this.backend = backend;
        }

        @Override
        public ZLinkChannelBackendAdapter createChannelAdapter(
            ZLinkBackendAdapterOptions options) {
            return backend;
        }

        @Override
        public ZLinkMonitoringBackendAdapter createMonitoringAdapter(
            ZLinkBackendAdapterOptions options) {
            return socket -> new ZLinkBackendSocketMonitor() {
                @Override
                public void onEvent(ZLinkBackendSocketMonitorHandler handler) {
                    handler.handle(new ZLinkBackendSocketMonitorEvent(
                        "CONNECTION_READY",
                        Optional.empty(),
                        "",
                        ""));
                }

                @Override
                public ZLinkBackendSocketMonitorEvent recv() {
                    return null;
                }

                @Override
                public String name() {
                    return "managed-local-monitor";
                }

                @Override
                public void close() {
                }
            };
        }

        @Override
        public ZLinkSpotBackendAdapter createSpotAdapter(
            ZLinkBackendAdapterOptions options) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkStreamBackendAdapter createStreamAdapter(
            ZLinkBackendAdapterOptions options) {
            throw new UnsupportedOperationException();
        }
    }

    private static final class ManagedLocalBackend
        implements ZLinkChannelBackendAdapter {
        private final FakeContext context = new FakeContext();
        private final FakeRouterSocket router = new FakeRouterSocket();
        private final String endpoint;
        private final List<ManagedAdmissionDealer> dealers = new ArrayList<>();

        private ManagedLocalBackend(String endpoint, int weight) {
            this.endpoint = endpoint;
            router.peerWeight = weight;
        }

        @Override
        public ZLinkBackendContext createContext() {
            return context;
        }

        @Override
        public ZLinkBackendDealerSocket createDealerSocket(
            ZLinkBackendContext context) {
            ManagedAdmissionDealer dealer =
                new ManagedAdmissionDealer(endpoint, router.peerWeight);
            dealers.add(dealer);
            return dealer;
        }

        @Override
        public ZLinkBackendRouterSocket createRouterSocket(
            ZLinkBackendContext context) {
            return router;
        }

        @Override
        public ZLinkBackendPublisherSocket createPublisherSocket(
            ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkBackendSubscriberSocket createSubscriberSocket(
            ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }

        private ManagedAdmissionDealer admissionDealer() {
            return dealers.stream()
                .filter(dealer -> dealer.admissionRequests == 1)
                .findFirst()
                .orElseThrow();
        }
    }

    private static final class ManagedAdmissionDealer
        implements ZLinkBackendDealerSocket {
        private final ArrayDeque<ZLinkBackendReceived> inbound =
            new ArrayDeque<>();
        private final String endpoint;
        private final RoutingId serverRid = RoutingId.from("local-server");
        private final int initialWeight;
        private final List<String> connected = new ArrayList<>();
        private int admissionRequests;

        private ManagedAdmissionDealer(String endpoint, int initialWeight) {
            this.endpoint = endpoint;
            this.initialWeight = initialWeight;
        }

        @Override
        public void setChannelName(String channelName) {
        }

        @Override
        public void bind(String endpoint) {
        }

        @Override
        public void connect(String endpoint) {
            connected.add(endpoint);
        }

        @Override
        public void disconnect(String endpoint) {
        }

        @Override
        public CompletionStage<Void> send(List<Message> parts) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<ZLinkBackendReceived> request(
            List<Message> parts,
            Duration timeout) {
            admissionRequests++;
            Message response = Message.from(
                ZLinkClientServerServiceWire.encodeAdmit(
                    descriptorWith(
                        1, initialWeight, ZLinkFrameworkRuntimeState.SERVING),
                    Integer.MAX_VALUE));
            return CompletableFuture.completedFuture(new ZLinkBackendReceived(
                ZLinkBackendRequestResult.OK,
                Optional.empty(),
                Optional.empty(),
                Optional.empty(),
                List.of(response)));
        }

        @Override
        public ZLinkBackendReceived recv(ZLinkBackendRecvMode mode) {
            return inbound.pollFirst();
        }

        @Override
        public boolean waitForReadable(Duration timeout) {
            return !inbound.isEmpty();
        }

        private ZLinkClientServerServerDescriptor descriptorWith(
            long revision,
            int weight,
            ZLinkFrameworkRuntimeState state) {
            return new ZLinkClientServerServerDescriptor(
                "orders",
                serverRid,
                1,
                revision,
                endpoint,
                weight,
                state,
                "default",
                "local",
                1,
                Instant.EPOCH);
        }

        private void enqueueUpdate(
            ZLinkClientServerServerDescriptor descriptor) {
            inbound.addLast(new ZLinkBackendReceived(
                Optional.empty(),
                Optional.empty(),
                Optional.empty(),
                List.of(Message.from(
                    ZLinkClientServerServiceWire.encodeUpdate(
                        descriptor, Integer.MAX_VALUE)))));
        }

        @Override
        public String name() {
            return "managed-local-dealer";
        }

        @Override
        public void close() {
            while (!inbound.isEmpty()) {
                inbound.removeFirst().close();
            }
        }
    }

    private static final class FakeChannelBackendAdapter implements ZLinkChannelBackendAdapter {
        final FakeContext context = new FakeContext();
        final FakeDealerSocket dealer = new FakeDealerSocket();
        final FakeRouterSocket router = new FakeRouterSocket();
        final FakeSpotRouteBridge bridge = new FakeSpotRouteBridge();
        final FakeSpotNode spotNode = new FakeSpotNode(bridge);

        @Override
        public ZLinkBackendContext createContext() {
            return context;
        }

        @Override
        public ZLinkBackendDealerSocket createDealerSocket(ZLinkBackendContext context) {
            return dealer;
        }

        @Override
        public ZLinkBackendRouterSocket createRouterSocket(ZLinkBackendContext context) {
            return router;
        }

        @Override
        public ZLinkBackendPublisherSocket createPublisherSocket(ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkBackendSubscriberSocket createSubscriberSocket(ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }
    }

    private static final class FakeDealerSocket implements ZLinkBackendDealerSocket,
        ZLinkTestAdmissionFactory.Backend {
        final List<String> connected = new ArrayList<>();
        final List<String> disconnected = new ArrayList<>();
        List<Message> lastSendParts = List.of();
        ZLinkBackendRequestResult requestResult = ZLinkBackendRequestResult.OK;
        int requestAttempts;
        int requestFailuresRemaining;
        final List<String> requestThreads = new CopyOnWriteArrayList<>();
        List<Message> requestReplyParts = List.of();

        @Override public void setChannelName(String channelName) { }
        @Override public void bind(String endpoint) { }
        @Override public void connect(String endpoint) { connected.add(endpoint); }
        @Override public void disconnect(String endpoint) { disconnected.add(endpoint); }
        @Override public CompletionStage<Void> send(List<Message> parts) {
            lastSendParts.forEach(Message::close);
            lastSendParts = parts.stream().map(Message::from).toList();
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<ZLinkBackendReceived> request(
            List<Message> parts,
            Duration timeout) {
            requestAttempts++;
            requestThreads.add(Thread.currentThread().getName());
            if (requestFailuresRemaining > 0) {
                requestFailuresRemaining--;
                return CompletableFuture.failedFuture(
                    new ZlinkSubmitException(SubmitResult.NOT_CONNECTED, 107));
            }
            return CompletableFuture.completedFuture(new ZLinkBackendReceived(
                requestResult,
                Optional.empty(),
                Optional.empty(),
                Optional.empty(),
                requestReplyParts.stream().map(Message::from).toList()));
        }
        @Override public ZLinkBackendReceived recv(ZLinkBackendRecvMode mode) { return null; }
        @Override public boolean waitForReadable(Duration timeout) { return false; }
        @Override public String name() { return "fake-dealer"; }
        @Override public void close() { lastSendParts.forEach(Message::close); }
    }

    private static final class FakeContext implements ZLinkBackendContext {
        @Override
        public void shutdown() {
        }

        @Override
        public String name() {
            return "fake-context";
        }

        @Override
        public void close() {
        }
    }

    private static final class FakeRouterSocket implements ZLinkBackendRouterSocket {
        final ArrayDeque<ZLinkBackendReceived> inbound = new ArrayDeque<>();
        long maxMessageSize;
        int peerWeight = 100;
        int replyCount;
        int requestAttempts;
        int requestFailuresRemaining;
        final List<String> requestThreads = new CopyOnWriteArrayList<>();
        ZLinkBackendRequestResult requestResult = ZLinkBackendRequestResult.OK;
        RoutingId connectRoutingId;
        List<Message> requestReplyParts = List.of();

        @Override public void setChannelName(String channelName) { }
        @Override public void setRoutingId(RoutingId routingId) { }
        @Override public void setConnectRoutingId(RoutingId routingId) { connectRoutingId = routingId; }
        @Override public void setProbe(boolean enabled) { }
        @Override public long maxMessageSize() { return maxMessageSize; }
        @Override public void setMaxMessageSize(long value) { maxMessageSize = value; }
        @Override public int peerWeight() { return peerWeight; }
        @Override public void setPeerWeight(int weight) { peerWeight = weight; }
        @Override public void bind(String endpoint) { }
        @Override public void connect(String endpoint) { }
        @Override public void disconnect(String endpoint) { }
        @Override public ZLinkBackendReceived recv(ZLinkBackendRecvMode mode) { return inbound.poll(); }
        @Override public boolean waitForReadable(Duration timeout) { return !inbound.isEmpty(); }
        @Override public CompletionStage<Void> send(
            RoutingId routingId,
            List<Message> parts) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<ZLinkBackendReceived> request(
            RoutingId routingId,
            List<Message> parts,
            Duration timeout) {
            requestAttempts++;
            requestThreads.add(Thread.currentThread().getName());
            if (requestFailuresRemaining > 0) {
                requestFailuresRemaining--;
                return CompletableFuture.failedFuture(
                    new ZlinkSubmitException(SubmitResult.NOT_CONNECTED, 107));
            }
            return CompletableFuture.completedFuture(new ZLinkBackendReceived(
                requestResult,
                Optional.empty(),
                Optional.empty(),
                Optional.empty(),
                requestReplyParts.stream().map(Message::from).toList()));
        }
        @Override public void reply(RoutingId routingId, long requestSeq, List<Message> parts) { replyCount++; }
        @Override public String name() { return "fake-router"; }
        @Override public void close() { }
    }

    private static final class FakeSpotRouteBridge implements ZLinkBackendSpotRouteBridge {
        boolean nativeCallbackInvoked;
        boolean routerReceivedInvoked;
        int drains;
        boolean completeRequests = true;
        boolean consumeRouterReceived;
        ZLinkBackendRequestResult requestResult = ZLinkBackendRequestResult.OK;
        List<Message> requestReplyParts = List.of();
        private CompletableFuture<List<Message>> pendingRequest;
        String lastChannelName;
        RoutingId lastTargetNodeRid;
        String lastTargetSpotId;
        List<Message> lastParts = List.of();
        int drainFailuresRemaining;
        int noDataDrainsRemaining;
        CountDownLatch firstDrainFailed;
        CountDownLatch nextDrainAfterFailure;
        boolean acceptSends = true;
        int sendAttempts;
        Runnable onSend;
        List<Message> lastAttemptParts = List.of();

        @Override public void attachRouterChannel(String channelName, ZLinkBackendRouterSocket router) { }
        @Override public CompletionStage<Void> send(
            String channelName,
            RoutingId targetNodeRid,
            String targetSpotId,
            List<Message> parts) {
            sendAttempts++;
            lastAttemptParts = List.copyOf(parts);
            recordBridgeCall(channelName, targetNodeRid, targetSpotId, parts);
            if (onSend != null) {
                onSend.run();
            }
            return acceptSends
                ? CompletableFuture.completedFuture(null)
                : CompletableFuture.failedFuture(
                    new ZlinkSubmitException(SubmitResult.BACKPRESSURED));
        }
        @Override public CompletionStage<List<Message>> request(
            String channelName,
            RoutingId targetNodeRid,
            String targetSpotId,
            List<Message> parts,
            Duration timeout) {
            recordBridgeCall(channelName, targetNodeRid, targetSpotId, parts);
            if (!completeRequests) {
                pendingRequest = new CompletableFuture<>();
                return pendingRequest;
            }
            if (requestResult != ZLinkBackendRequestResult.OK) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "SPOT route bridge request failed: " + requestResult));
            }
            return CompletableFuture.completedFuture(requestReplyParts);
        }
        void completePendingRequest(List<Message> reply) {
            if (pendingRequest == null) {
                throw new AssertionError("no pending backend route request");
            }
            pendingRequest.complete(reply);
        }
        private void recordBridgeCall(
            String channelName,
            RoutingId targetNodeRid,
            String targetSpotId,
            List<Message> parts) {
            lastChannelName = channelName;
            lastTargetNodeRid = targetNodeRid;
            lastTargetSpotId = targetSpotId;
            lastParts.forEach(Message::close);
            lastParts = parts.stream()
                .map(Message::toByteArray)
                .map(Message::from)
                .toList();
        }
        @Override public boolean handleRouterReceived(String channelName, RoutingId sourceNodeRid, long requestSeq, List<Message> parts) {
            routerReceivedInvoked = true;
            return consumeRouterReceived;
        }
        @Override public int drain() {
            drains++;
            if (noDataDrainsRemaining > 0) {
                noDataDrainsRemaining--;
                if (firstDrainFailed != null) {
                    firstDrainFailed.countDown();
                }
                throw new ZlinkRecvException(RecvResult.NO_DATA);
            }
            if (drainFailuresRemaining > 0) {
                drainFailuresRemaining--;
                if (firstDrainFailed != null) {
                    firstDrainFailed.countDown();
                }
                throw new IllegalStateException("synthetic drain failure");
            }
            if (firstDrainFailed != null
                && firstDrainFailed.getCount() == 0
                && nextDrainAfterFailure != null) {
                nextDrainAfterFailure.countDown();
            }
            return drains;
        }
        @Override public String name() { return "fake-bridge"; }
        @Override public void close() { lastParts.forEach(Message::close); }
    }

    private record TestRequest(String value) {
    }

    private record TestReply(String value) {
    }

    private static class BaseOutbound {
    }

    private static final class DerivedOutbound extends BaseOutbound {
    }

    private record OutboundMarkerSerializer(String marker)
        implements ZLinkMessageSerializer {
        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            return ZLinkEncodedPayload.from(marker.getBytes(StandardCharsets.UTF_8));
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            throw new UnsupportedOperationException();
        }
    }

    private static final class FakeSpotNode implements ZLinkInternalSpotNode,
        ZLinkTestAdmissionFactory.Backend {
        private final FakeSpotRouteBridge bridge;
        private final FakeSpot entrySpot = new FakeSpot();
        private volatile byte[] lastMetadata = new byte[0];
        private final CompletableFuture<Void> metadataObserved =
            new CompletableFuture<>();
        private int requestAttempts;
        private int requestFailuresRemaining;
        private SubmitResult requestFailureResult = SubmitResult.NOT_CONNECTED;
        private Optional<Integer> channelTargetClassification = Optional.empty();
        private final ArrayDeque<Integer> localNodeStatuses =
            new ArrayDeque<>();
        private Consumer<ZLinkBackendAdmissionKey> admissionReady =
            ignored -> { };
        private Duration admissionTimeout = Duration.ofSeconds(1);
        private int localNodeAttempts;

        FakeSpotNode(FakeSpotRouteBridge bridge) {
            this.bridge = bridge;
        }

        @Override public RoutingId routingId() { return RoutingId.from("owner-node"); }
        @Override public void setRoutingId(RoutingId routingId) { }
        @Override public void setPublisherRoutingId(RoutingId routingId) { }
        @Override public void setSubscriberRoutingId(RoutingId routingId) { }
        @Override public void setRouterBind(String endpoint) { }
        @Override public void setPubBind(String endpoint) { }
        @Override public void connectPeer(String endpoint) { }
        @Override public void connectPeer(RoutingId peerRid, String endpoint) { }
        @Override public void disconnectPeer(String endpoint) { }
        @Override public void disconnectPeer(RoutingId peerRid) { }
        @Override public ZLinkBackendSpotRouteBridge createRouteBridge() { return bridge; }
        @Override public ZLinkBackendSpot createSpot() { throw new UnsupportedOperationException(); }
        @Override public ZLinkBackendSpot entrySpot() { return entrySpot; }
        @Override public Duration admissionTimeout() { return admissionTimeout; }
        @Override public void setAdmissionReadyHandler(
            Consumer<ZLinkBackendAdmissionKey> handler) {
            admissionReady = handler;
        }
        @Override public Optional<CompletionStage<Integer>> submitLocalNodeSend(
            RoutingId sourceNodeRid,
            byte[] metadata,
            List<Message> parts) {
            localNodeAttempts++;
            Integer status = localNodeStatuses.isEmpty()
                ? 0
                : localNodeStatuses.removeFirst();
            return Optional.of(CompletableFuture.completedFuture(status));
        }
        @Override public Optional<Integer> classifyChannelTarget(String channelName) {
            return channelTargetClassification;
        }
        void signalLocalNodeReady() {
            admissionReady.accept(ZLinkBackendAdmissionKey.node(routingId()));
        }
        @Override public CompletionStage<Void> sendToNode(
            RoutingId targetNodeRid,
            byte[] metadata,
            List<Message> parts) {
            lastMetadata = metadata.clone();
            metadataObserved.complete(null);
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<ZLinkBackendReceived> requestToNode(
            RoutingId targetNodeRid,
            byte[] metadata,
            List<Message> parts,
            Duration timeout) {
            lastMetadata = metadata.clone();
            return CompletableFuture.completedFuture(new ZLinkBackendReceived(
                Optional.empty(),
                Optional.empty(),
                Optional.empty(),
                List.of(Message.from("{\"value\":\"reply\"}".getBytes()))));
        }
        @Override public CompletionStage<Void> sendToChannel(
            String channelName,
            byte[] metadata,
            List<Message> parts) {
            lastMetadata = metadata.clone();
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<ZLinkBackendReceived> requestToChannel(
            String channelName,
            byte[] metadata,
            List<Message> parts,
            Duration timeout) {
            requestAttempts++;
            if (requestFailuresRemaining > 0) {
                requestFailuresRemaining--;
                return CompletableFuture.failedFuture(
                    new ZlinkSubmitException(requestFailureResult, 2));
            }
            lastMetadata = metadata.clone();
            return CompletableFuture.completedFuture(new ZLinkBackendReceived(
                Optional.empty(),
                Optional.empty(),
                Optional.empty(),
                List.of(Message.from("{\"value\":\"reply\"}".getBytes()))));
        }
        @Override public ZLinkBackendActorRef createActor(String actorId, Message createRequest) { throw new UnsupportedOperationException(); }
        @Override public ZLinkBackendActorRef actorLookup(String actorId) { throw new UnsupportedOperationException(); }
        @Override public CompletionStage<ZLinkBackendActorJoinResult> joinActor(ZLinkBackendActorRef actor, RoutingId targetNodeRid, String targetSpotId, List<Message> parts, Duration timeout) { throw new UnsupportedOperationException(); }
        @Override public CompletionStage<ZLinkBackendActorJoinEntrySpotResult> joinActorEntrySpot(ZLinkBackendActorRef actor, RoutingId targetNodeRid, Message request, Duration timeout) { throw new UnsupportedOperationException(); }
        @Override public CompletionStage<List<Message>> leaveActor(ZLinkBackendActorRef actor, String currentSpotId, Duration timeout) { throw new UnsupportedOperationException(); }
        @Override public CompletionStage<Void> destroyActor(ZLinkBackendActorRef actor, Duration timeout) { throw new UnsupportedOperationException(); }
        @Override public boolean sendActorBoundSession(ZLinkBackendActorRef actor, List<Message> parts, SendFlags flags) { return false; }
        @Override public void replyActorNoBind(ZLinkBackendActorRef actor, RoutingId sourceNodeRid, RoutingId sourceSessionRid, long requestId, int flags, List<Message> parts) { throw new UnsupportedOperationException(); }
        @Override public boolean sendToActor(ZLinkBackendActorRef actor, List<Message> parts, SendFlags flags) { return false; }
        @Override public CompletionStage<List<Message>> requestToActor(ZLinkBackendActorRef actor, List<Message> parts, SendFlags flags, Duration timeout) { throw new UnsupportedOperationException(); }
        @Override public boolean forwardActorBoundSession(ZLinkBackendActorRef actor, RoutingId sourceNodeRid, RoutingId sourceSessionRid, List<Message> parts, SendFlags flags) { return false; }
        @Override public void bindRemoteActorBoundSession(ZLinkBackendActorRef actor, RoutingId sourceNodeRid, RoutingId sourceSessionRid) { }
        @Override public void closeActorBoundSession(ZLinkBackendActorRef actor, Duration timeout) { }
        @Override public String name() { return "fake-node"; }
        @Override public void close() { }
    }

    private static final class FakeSpot implements ZLinkBackendSpot {
        int sendAttempts;
        int sendFailuresRemaining;
        SubmitResult sendFailureResult = SubmitResult.BACKPRESSURED;
        SendFlags lastSendFlags;
        int requestAttempts;
        int requestFailuresRemaining;
        SubmitResult requestFailureResult = SubmitResult.BACKPRESSURED;
        ZLinkBackendRequestResult requestResult = ZLinkBackendRequestResult.OK;
        final ArrayDeque<ZLinkBackendRequestResult> requestResults =
            new ArrayDeque<>();
        SendFlags lastRequestFlags;
        long lastSpotGeneration;
        int authorityRemembers;
        RoutingId authorityNodeRid;
        String authoritySpotId;
        long authoritySpotGeneration;
        long authorityOwnerGeneration;
        long authorityOwnerLeaseGeneration;
        List<Message> requestReplyParts = List.of();

        @Override public String spotId() { return "entry-spot"; }
        @Override public void setRoutingId(String spotId) { }
        @Override public void setSubscription(String topic) { }
        @Override public ZLinkBackendTopicMessage subscribe(ZLinkBackendRecvMode mode) { return null; }
        @Override public ZLinkBackendReceived recvRoute(ZLinkBackendRecvMode mode) { return null; }
        @Override public void rememberSpotAuthority(
            RoutingId targetNodeRid,
            String spotId,
            long objectGeneration,
            long authorityOwnerGeneration,
            long ownerLeaseGeneration) {
            authorityRemembers++;
            authorityNodeRid = targetNodeRid;
            authoritySpotId = spotId;
            authoritySpotGeneration = objectGeneration;
            this.authorityOwnerGeneration = authorityOwnerGeneration;
            this.authorityOwnerLeaseGeneration = ownerLeaseGeneration;
        }
        @Override public boolean publish(String channelName, String topic, List<Message> parts, SendFlags flags) { return true; }
        @Override public CompletionStage<Void> publishAsync(String channelName, String topic, List<Message> parts, SendFlags flags) { return CompletableFuture.completedFuture(null); }
        @Override public CompletionStage<Void> sendToSpot(
            RoutingId targetNodeRid,
            String spotId,
            long spotGeneration,
            List<Message> parts) {
            lastSpotGeneration = spotGeneration;
            sendAttempts++;
            lastSendFlags = SendFlags.NONE;
            if (sendFailuresRemaining > 0) {
                sendFailuresRemaining--;
                return CompletableFuture.failedFuture(
                    new ZlinkSubmitException(sendFailureResult));
            }
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<ZLinkBackendReceived> requestToSpot(
            RoutingId targetNodeRid,
            String spotId,
            long spotGeneration,
            List<Message> parts,
            Duration timeout) {
            lastSpotGeneration = spotGeneration;
            requestAttempts++;
            lastRequestFlags = SendFlags.NONE;
            if (requestFailuresRemaining > 0) {
                requestFailuresRemaining--;
                return CompletableFuture.failedFuture(
                    new ZlinkSubmitException(requestFailureResult));
            }
            ZLinkBackendRequestResult result = requestResults.isEmpty()
                ? requestResult
                : requestResults.removeFirst();
            return CompletableFuture.completedFuture(new ZLinkBackendReceived(
                result,
                Optional.empty(),
                Optional.of(spotId),
                Optional.empty(),
                requestReplyParts.stream().map(Message::from).toList()));
        }
        @Override public void onDispatchEvent(ZLinkBackendSpotDispatchHandler handler) { }
        @Override public ZLinkBackendActorJoinRequest recvActorJoin(ZLinkBackendRecvMode mode) { return null; }
        @Override public void replyActorJoin(ZLinkBackendActorJoinRequest request, int joinResultCode, List<Message> parts) { }
        @Override public ZLinkBackendActorLifecycleEvent recvActorLifecycle(ZLinkBackendRecvMode mode) { return null; }
        @Override public String name() { return "fake-spot"; }
        @Override public void close() { requestReplyParts.forEach(Message::close); }
    }
}
