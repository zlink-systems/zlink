package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.spots.SpotHandles;

import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.assertThrows;

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
import systems.zlink.framework.ZLinkMessageContext;

import systems.zlink.framework.configuration.ZLinkEndpointConnections;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.runtime.internal.spots.ZLinkInstanceSpotCallRuntime;

final class ZLinkChannelRuntimeTest {
    private static ZLinkHandlerActivator handlers() {
        SpotTransportAddressResolver resolver = spotId ->
            java.util.concurrent.CompletableFuture.completedFuture(Optional.of(
                new SpotTransportAddress(
                    "play.route",
                    RoutingId.from("play-node"),
                    spotId,
                    1L,
                    systems.zlink.framework.spots.ZLinkSpotKind.USER)));
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
        var request = new systems.zlink.framework.runtime.channels.DefaultRequestContext(
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
    void routeRequestTreatsMissingChannelMemberAsTerminalSubmitFailure() {
        assertFalse(ZLinkChannelRequestSubmitter.isRetriableSubmit(
            new ZlinkSubmitException(SubmitResult.NOT_FOUND, 2)));
        assertFalse(ZLinkChannelRequestSubmitter.isRetriableSubmit(
            new ZlinkSubmitException(SubmitResult.NOT_FOUND, 113)));
    }

    @Test
    void meshChannelRequestPreservesPayloadAcrossReadinessRetry() {
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

            TestReply reply = runtime.requestToChannel(
                    "play",
                    new TestRequest("hello"))
                .submit(TestReply.class).toCompletableFuture().join();

            assertEquals("reply", reply.value());
            assertEquals(2, backend.spotNode.requestAttempts);
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

            ExecutionException error = org.junit.jupiter.api.Assertions.assertThrows(
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
        DefaultRequestContext context = new DefaultRequestContext();
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
    void routeBridgeRawRequestCompletesWhenRouteLoopReceivesRawReply() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
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
                List.of(Message.from("raw-request".getBytes())),
                Duration.ofMillis(300));

            backend.router.inbound.add(new ZLinkBackendReceived(
                Optional.of(RoutingId.from("play-node")),
                Optional.empty(),
                Optional.empty(),
                List.of(Message.from("{\"ok\":true,\"response\":{\"value\":\"reply\"}}".getBytes()))));

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
        systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
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
            assertEquals("TestRequest", backend.bridge.lastParts.get(0).toUtf8String());
        }
    }

    @Test
    void routeClientSpotSendUsesRouteBridge() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
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
            assertEquals("TestRequest", backend.bridge.lastParts.get(0).toUtf8String());
        }
    }

    @Test
    void routeReceiveLoopSkipsRouterProbeFrame() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
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
    void channelRequestRetriesRetriableSubmitFailure() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        options.addClientServerChannel("profile")
            .client()
            .connect("inproc://profile");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.dealer.requestFailuresRemaining = 1;
        backend.dealer.requestReplyParts = List.of(Message.from("{\"value\":\"reply\"}".getBytes()));
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            TestReply reply = runtime.requestToChannel("profile", new TestRequest("hello"))
                .timeout(Duration.ofMillis(300))
                .submit(TestReply.class).toCompletableFuture().join();

            assertEquals("reply", reply.value());
            assertEquals(2, backend.dealer.requestAttempts);
        }
    }

    @Test
    void routeRequestRetriesRetriableSubmitFailure() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route")
            .enableClient("inproc://play-peer");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.router.requestFailuresRemaining = 1;
        backend.router.requestReplyParts = List.of(Message.from("{\"value\":\"reply\"}".getBytes()));
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            TestReply reply = runtime.requestToNode(
                    "play.route",
                    RoutingId.from("play-node"),
                    new TestRequest("hello"))
                .timeout(Duration.ofMillis(300))
                .submit(TestReply.class).toCompletableFuture().join();

            assertEquals("reply", reply.value());
            assertEquals(2, backend.router.requestAttempts);
        }
    }

    @org.junit.jupiter.params.ParameterizedTest
    @org.junit.jupiter.params.provider.EnumSource(
        value = ZLinkBackendRequestResult.class,
        names = {"TIMED_OUT", "NOT_CONNECTED", "TERMINATED"})
    void routeRequestDeliversTerminalCallbackResultWithoutResubmitting(
        ZLinkBackendRequestResult terminalResult) throws Exception {
        var scheduler = java.util.concurrent.Executors.newSingleThreadScheduledExecutor();
        FakeRouterSocket router = new FakeRouterSocket();
        router.requestResult = terminalResult;
        router.requestReplyParts = List.of(Message.from("same-payload".getBytes()));
        var submitter = new ZLinkChannelRequestSubmitter(
            scheduler, Duration.ofMillis(300));
        CompletableFuture<ZLinkBackendRequestResult> completion =
            new CompletableFuture<>();
        try {
            submitter.submitRoute(
                router,
                RoutingId.from("play-node"),
                List.of(Message.from("same-payload".getBytes())),
                reply -> {
                    completion.complete(reply.result());
                    reply.parts().forEach(Message::close);
                },
                Duration.ofMillis(300),
                new CompletableFuture<>());

            assertEquals(terminalResult, completion.get(1, TimeUnit.SECONDS));
            Thread.sleep(40);
            assertEquals(1, router.requestAttempts);
        } finally {
            router.close();
            scheduler.shutdownNow();
        }
    }

    @Test
    void routeRequestAcceptsReplyThatEqualsTheOriginalPayload() throws Exception {
        var scheduler = java.util.concurrent.Executors.newSingleThreadScheduledExecutor();
        FakeRouterSocket router = new FakeRouterSocket();
        router.requestResult = ZLinkBackendRequestResult.OK;
        router.requestReplyParts = List.of(Message.from("same-payload".getBytes()));
        var submitter = new ZLinkChannelRequestSubmitter(
            scheduler, Duration.ofMillis(300));
        CompletableFuture<ZLinkBackendRequestResult> completion =
            new CompletableFuture<>();
        try {
            submitter.submitRoute(
                router,
                RoutingId.from("play-node"),
                List.of(Message.from("same-payload".getBytes())),
                reply -> {
                    completion.complete(reply.result());
                    reply.parts().forEach(Message::close);
                },
                Duration.ofMillis(300),
                new CompletableFuture<>());

            assertEquals(ZLinkBackendRequestResult.OK, completion.get(1, TimeUnit.SECONDS));
            Thread.sleep(40);
            assertEquals(1, router.requestAttempts);
        } finally {
            router.close();
            scheduler.shutdownNow();
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
                new java.util.LinkedHashMap<>(Map.of("tenant", "blue"));
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
    void localNodeSendWaitsForTheExactCapacitySignalWithoutPublicRetry() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.spotNode.localNodeStatuses.add(1);
        backend.spotNode.localNodeStatuses.add(0);
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

            assertFalse(result.isDone());
            assertEquals(1, backend.spotNode.localNodeAttempts);
            backend.spotNode.signalLocalNodeReady();
            assertEquals(0, systems.zlink.framework.runtime.messaging.OneWayTestStatus.status(result));
            assertEquals(2, backend.spotNode.localNodeAttempts);
        }
    }

    @Test
    void localNodeTimeoutPreventsLateAdmission() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.spotNode.localNodeStatuses.add(1);
        backend.spotNode.admissionTimeout = Duration.ofNanos(1);
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
                    new TestRequest("timeout"))
                .submit();

            assertEquals(
                2,
                systems.zlink.framework.runtime.messaging.OneWayTestStatus.status(result));
            backend.spotNode.signalLocalNodeReady();
            assertEquals(1, backend.spotNode.localNodeAttempts);
        }
    }

    @Test
    void spotRouterNodeRequestRetriesUntilConnectedRouteIsReady() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.spotNode.entrySpot.requestFailuresRemaining = 1;
        backend.spotNode.entrySpot.requestReplyParts = List.of(
            Message.from("{\"value\":\"reply\"}".getBytes()));
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            runtime.registerSpotRouterNode("play.route", backend.spotNode);

            TestReply reply = runtime.requestToSpot(
                    "room-spot",
                    new TestRequest("hello"))
                .timeout(Duration.ofMillis(300))
                .submit(TestReply.class).toCompletableFuture().join();

            assertEquals("reply", reply.value());
            assertEquals(2, backend.spotNode.entrySpot.requestAttempts);
            assertEquals(SendFlags.NONE, backend.spotNode.entrySpot.lastRequestFlags);
            assertEquals(1L, backend.spotNode.entrySpot.lastSpotGeneration);
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

            var failure = org.junit.jupiter.api.Assertions.assertThrows(
                java.util.concurrent.CompletionException.class,
                () -> runtime.requestToSpot(
                    "room-spot",
                    new TestRequest("hello"))
                .timeout(Duration.ofMillis(300))
                .submit(TestReply.class)
                .toCompletableFuture()
                .join());

            var frameworkError = assertInstanceOf(
                systems.zlink.framework.errors.ZLinkFrameworkException.class,
                failure.getCause());
            assertEquals(
                systems.zlink.framework.errors.ZLinkFrameworkErrorKind.UNAVAILABLE,
                frameworkError.kind());
            assertEquals(1, backend.spotNode.entrySpot.requestAttempts);
        }
    }

    @Test
    void spotRouterNodeSendRetriesUntilConnectedRouteIsReady() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.spotNode.entrySpot.sendFailuresRemaining = 1;
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            runtime.registerSpotRouterNode("play.route", backend.spotNode);

            runtime.sendToSpot(
                    "room-spot",
                    new TestRequest("hello"))
                .submit().toCompletableFuture().join();

            assertEquals(2, backend.spotNode.entrySpot.sendAttempts);
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

            CompletionException error = org.junit.jupiter.api.Assertions.assertThrows(
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
    void staleSpotReplyInvalidatesRouteBeforeTheNextSpotRequest() {
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
            public java.util.concurrent.CompletionStage<Optional<SpotTransportAddress>>
                resolve(String spotId) {
                long generation = resolves.incrementAndGet();
                return CompletableFuture.completedFuture(Optional.of(
                    new SpotTransportAddress(
                        "play.route",
                        RoutingId.from("play-node"),
                        spotId,
                        generation,
                        systems.zlink.framework.spots.ZLinkSpotKind.USER)));
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

            CompletionException failure = assertThrows(
                CompletionException.class,
                () -> runtime.requestToSpot(
                        "room-spot",
                        new TestRequest("stale"))
                    .timeout(Duration.ofMillis(300))
                    .submit(TestReply.class)
                    .toCompletableFuture()
                    .join());
            assertEquals(
                ZLinkFrameworkErrorKind.NOT_FOUND,
                assertInstanceOf(ZLinkFrameworkException.class, failure.getCause())
                    .kind());

            TestReply reply = runtime.requestToSpot(
                    "room-spot",
                    new TestRequest("fresh"))
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
    void instanceSpotRequestReactivatesAfterStaleOwnerNotFound() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.spotNode.entrySpot.requestResult = ZLinkBackendRequestResult.NOT_FOUND;
        try (ZLinkChannelRuntime runtime = new ZLinkChannelRuntime(
            backend,
            options.registration(),
            new ZLinkJsonMessageSerializer(), handlers())) {
            runtime.registerSpotRouterNode("play.route", backend.spotNode);
            runtime.registerInstanceSpotCallRuntime(new ZLinkInstanceSpotCallRuntime() {
                @Override
                public java.util.concurrent.CompletionStage<Void> send(
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
                public java.util.concurrent.CompletionStage<List<Message>> request(
                    String spotId,
                    String stableType,
                    String meshName,
                    Message payload,
                    Optional<String> packetName,
                    String contentType,
                    Map<String, String> metadata,
                    Duration timeout) {
                    byte[] reply = new ZLinkJsonMessageSerializer()
                        .serialize(new TestReply("reactivated"))
                        .bytes();
                    return CompletableFuture.completedFuture(List.of(Message.from(reply)));
                }
            });

            TestReply reply = runtime.requestToSpot(
                    "room-spot",
                    new TestRequest("hello"))
                .instanceSpot("room")
                .inMesh("play.route")
                .submit(TestReply.class)
                .toCompletableFuture()
                .join();

            assertEquals("reactivated", reply.value());
            assertEquals(1, backend.spotNode.entrySpot.requestAttempts);
        }
    }

    @Test
    void routeBridgeRawRequestConsumesNativeReplyEvenWhenRequestSeqIsPresent() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
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
                List.of(Message.from("raw-request".getBytes())),
                Duration.ofMillis(300));

            backend.router.inbound.add(new ZLinkBackendReceived(
                Optional.of(RoutingId.from("play-node")),
                Optional.empty(),
                Optional.of(7L),
                List.of(Message.from("{\"ok\":true,\"response\":{\"value\":\"reply\"}}".getBytes()))));

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
    void routeBridgeRawRequestUnwrapsRoutedSpotEnvelopeReply() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
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
                Optional.of(7L),
                List.of(
                    Message.from("__zlink.routed_spot.egress.request".getBytes()),
                    Message.from("StateReply".getBytes()),
                    Message.from("{\"spotId\":\"room-spot\",\"value\":\"pong\"}".getBytes()))));

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
        systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
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
        systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
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
    void routeBridgeRequestIgnoresNativeRequestEchoAndWaitsForRouterReply() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.bridge.requestReplyParts = List.of(
            Message.from("__zlink.routed_spot.egress.request".getBytes()),
            Message.from("StateRequest".getBytes()));
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
                Optional.of(7L),
                List.of(
                    Message.from("__zlink.routed_spot.egress.request".getBytes()),
                    Message.from("StateReply".getBytes()),
                    Message.from("{\"spotId\":\"room-spot\",\"value\":\"pong\"}".getBytes()))));

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
    void routeBridgeRequestIgnoresRouterRequestEchoAndWaitsForRouterReply() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
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
    void routeBridgeLateRawReplyAfterNativeCompletionIsNotRepliedAsMissingRouteHandler()
        throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
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
            assertEquals(0, backend.router.replyCount);
        }
    }

    @Test
    void routeBridgeRawRequestCompletesBeforeBridgeConsumesActorJoinReply() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.bridge.completeRequests = false;
        backend.bridge.consumeRouterReceived = true;
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

            backend.router.inbound.add(new ZLinkBackendReceived(
                Optional.of(RoutingId.from("play-node")),
                Optional.empty(),
                Optional.empty(),
                List.of(Message.from("true\nactor-node\nplayer-x\n1\ncmVwbHk=".getBytes()))));

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
    void routeBridgeRawRequestFailsOnFrameworkErrorReplyWithoutBridgeFeedback() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
            .enableServer("inproc://play-route");
        FakeChannelBackendAdapter backend = new FakeChannelBackendAdapter();
        backend.bridge.completeRequests = false;
        backend.bridge.consumeRouterReceived = true;
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

            backend.router.inbound.add(new ZLinkBackendReceived(
                Optional.of(RoutingId.from("play-node")),
                Optional.empty(),
                Optional.empty(),
                List.of(
                    Message.from("ZLinkFrameworkError".getBytes()),
                    Message.from("missing route handler".getBytes()))));

            ExecutionException error = org.junit.jupiter.api.Assertions.assertThrows(
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
        systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
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
    void routeBridgeDrainFailureDoesNotStopLaterRawReplyCompletion() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
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
    void routeBridgeNoDataDrainDoesNotStopLaterRawReplyCompletion() throws Exception {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofMillis(300));
        systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(options, "play.route")
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

            ExecutionException error = org.junit.jupiter.api.Assertions.assertThrows(
                ExecutionException.class,
                () -> request.toCompletableFuture().get(1, TimeUnit.SECONDS));
            assertInstanceOf(ZLinkFrameworkException.class, error.getCause());
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

            org.junit.jupiter.api.Assertions.assertThrows(
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

            org.junit.jupiter.api.Assertions.assertThrows(
                ZLinkConfigurationException.class,
                () -> socket.weight(-1));
            org.junit.jupiter.api.Assertions.assertThrows(
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

    private static final class DefaultRequestContext implements ZLinkMessageContext {
        @Override
        public java.util.Optional<String> meshName() {
            return java.util.Optional.empty();
        }

        @Override
        public java.util.Optional<String> channelName() {
            return java.util.Optional.of("profile");
        }

        @Override
        public String packetName() {
            return "Echo";
        }

        @Override
        public java.util.Optional<String> contentType() {
            return java.util.Optional.empty();
        }

        @Override
        public java.util.Map<String, String> metadata() {
            return java.util.Map.of();
        }

        @Override
        public java.util.Optional<String> correlationId() {
            return java.util.Optional.empty();
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
        public boolean send(List<Message> parts, SendFlags flags) {
            return true;
        }

        @Override
        public boolean request(
            List<Message> parts,
            ZLinkBackendRequestCallback callback,
            SendFlags flags,
            Duration timeout) {
            admissionRequests++;
            Message response = Message.from(
                ZLinkClientServerServiceWire.encodeAdmit(
                    descriptorWith(
                        1, initialWeight, ZLinkFrameworkRuntimeState.SERVING),
                    Integer.MAX_VALUE));
            callback.handle(new ZLinkBackendReceived(
                ZLinkBackendRequestResult.OK,
                Optional.empty(),
                Optional.empty(),
                Optional.empty(),
                List.of(response)));
            return true;
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

    private static final class FakeDealerSocket implements ZLinkBackendDealerSocket {
        final List<String> connected = new ArrayList<>();
        final List<String> disconnected = new ArrayList<>();
        ZLinkBackendRequestResult requestResult = ZLinkBackendRequestResult.OK;
        int requestAttempts;
        int requestFailuresRemaining;
        List<Message> requestReplyParts = List.of();

        @Override public void setChannelName(String channelName) { }
        @Override public void bind(String endpoint) { }
        @Override public void connect(String endpoint) { connected.add(endpoint); }
        @Override public void disconnect(String endpoint) { disconnected.add(endpoint); }
        @Override public boolean send(List<Message> parts, SendFlags flags) { return true; }
        @Override public boolean request(List<Message> parts, ZLinkBackendRequestCallback callback, SendFlags flags, Duration timeout) {
            requestAttempts++;
            if (requestFailuresRemaining > 0) {
                requestFailuresRemaining--;
                throw new IllegalStateException(
                    "transient submit",
                    new ZlinkSubmitException(SubmitResult.NOT_CONNECTED, 107));
            }
            callback.handle(new ZLinkBackendReceived(
                requestResult,
                Optional.empty(),
                Optional.empty(),
                Optional.empty(),
                requestReplyParts.stream().map(Message::from).toList()));
            return true;
        }
        @Override public ZLinkBackendReceived recv(ZLinkBackendRecvMode mode) { return null; }
        @Override public boolean waitForReadable(Duration timeout) { return false; }
        @Override public String name() { return "fake-dealer"; }
        @Override public void close() { }
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
        @Override public boolean send(RoutingId routingId, List<Message> parts, SendFlags flags) { return true; }
        @Override public boolean request(RoutingId routingId, List<Message> parts, ZLinkBackendRequestCallback callback, SendFlags flags, Duration timeout) {
            requestAttempts++;
            if (requestFailuresRemaining > 0) {
                requestFailuresRemaining--;
                throw new IllegalStateException(
                    "transient submit",
                    new ZlinkSubmitException(SubmitResult.NOT_CONNECTED, 107));
            }
            callback.handle(new ZLinkBackendReceived(
                requestResult,
                Optional.empty(),
                Optional.empty(),
                Optional.empty(),
                requestReplyParts.stream().map(Message::from).toList()));
            return true;
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
        String lastChannelName;
        RoutingId lastTargetNodeRid;
        String lastTargetSpotId;
        List<Message> lastParts = List.of();
        int drainFailuresRemaining;
        int noDataDrainsRemaining;
        CountDownLatch firstDrainFailed;
        CountDownLatch nextDrainAfterFailure;

        @Override public void attachRouterChannel(String channelName, ZLinkBackendRouterSocket router) { }
        @Override public boolean send(String channelName, RoutingId targetNodeRid, String targetSpotId, List<Message> parts, SendFlags flags) {
            recordBridgeCall(channelName, targetNodeRid, targetSpotId, parts);
            return true;
        }
        @Override public boolean request(String channelName, RoutingId targetNodeRid, String targetSpotId, List<Message> parts, ZLinkBackendRequestCallback callback, SendFlags flags, Duration timeout) {
            recordBridgeCall(channelName, targetNodeRid, targetSpotId, parts);
            if (!completeRequests) {
                return true;
            }
            callback.handle(new ZLinkBackendReceived(
                requestResult,
                Optional.empty(),
                Optional.of(targetSpotId),
                Optional.empty(),
                requestReplyParts));
            return true;
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

    private static final class FakeSpotNode implements ZLinkInternalSpotNode,
        systems.zlink.framework.runtime.host.ZLinkTestAdmissionFactory.Backend {
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
        private java.util.function.Consumer<ZLinkBackendAdmissionKey> admissionReady =
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
            java.util.function.Consumer<ZLinkBackendAdmissionKey> handler) {
            admissionReady = handler;
        }
        @Override public java.util.Optional<Integer> submitLocalNodeSend(
            RoutingId sourceNodeRid,
            byte[] metadata,
            List<Message> parts) {
            localNodeAttempts++;
            Integer status = localNodeStatuses.isEmpty()
                ? 0
                : localNodeStatuses.removeFirst();
            return java.util.Optional.of(status);
        }
        @Override public Optional<Integer> classifyChannelTarget(String channelName) {
            return channelTargetClassification;
        }
        void signalLocalNodeReady() {
            admissionReady.accept(ZLinkBackendAdmissionKey.node(routingId()));
        }
        @Override public boolean sendToNode(
            RoutingId targetNodeRid,
            byte[] metadata,
            List<Message> parts,
            SendFlags flags) {
            lastMetadata = metadata.clone();
            metadataObserved.complete(null);
            return true;
        }
        @Override public boolean requestToNode(
            RoutingId targetNodeRid,
            byte[] metadata,
            List<Message> parts,
            ZLinkBackendRequestCallback callback,
            SendFlags flags,
            Duration timeout) {
            lastMetadata = metadata.clone();
            callback.handle(new ZLinkBackendReceived(
                Optional.empty(),
                Optional.empty(),
                Optional.empty(),
                List.of(Message.from("{\"value\":\"reply\"}".getBytes()))));
            return true;
        }
        @Override public boolean sendToChannel(
            String channelName,
            byte[] metadata,
            List<Message> parts,
            SendFlags flags) {
            lastMetadata = metadata.clone();
            return true;
        }
        @Override public boolean requestToChannel(
            String channelName,
            byte[] metadata,
            List<Message> parts,
            ZLinkBackendRequestCallback callback,
            SendFlags flags,
            Duration timeout) {
            requestAttempts++;
            if (requestFailuresRemaining > 0) {
                requestFailuresRemaining--;
                throw new ZlinkSubmitException(requestFailureResult, 2);
            }
            lastMetadata = metadata.clone();
            callback.handle(new ZLinkBackendReceived(
                Optional.empty(),
                Optional.empty(),
                Optional.empty(),
                List.of(Message.from("{\"value\":\"reply\"}".getBytes()))));
            return true;
        }
        @Override public ZLinkBackendActorRef createActor(String actorId, Message createRequest) { throw new UnsupportedOperationException(); }
        @Override public ZLinkBackendActorRef actorLookup(String actorId) { throw new UnsupportedOperationException(); }
        @Override public java.util.concurrent.CompletionStage<ZLinkBackendActorJoinResult> joinActor(ZLinkBackendActorRef actor, RoutingId targetNodeRid, String targetSpotId, List<Message> parts, Duration timeout) { throw new UnsupportedOperationException(); }
        @Override public java.util.concurrent.CompletionStage<ZLinkBackendActorJoinEntrySpotResult> joinActorEntrySpot(ZLinkBackendActorRef actor, RoutingId targetNodeRid, Message request, Duration timeout) { throw new UnsupportedOperationException(); }
        @Override public java.util.concurrent.CompletionStage<List<Message>> leaveActor(ZLinkBackendActorRef actor, String currentSpotId, Duration timeout) { throw new UnsupportedOperationException(); }
        @Override public java.util.concurrent.CompletionStage<Void> destroyActor(ZLinkBackendActorRef actor, Duration timeout) { throw new UnsupportedOperationException(); }
        @Override public boolean sendActorBoundSession(ZLinkBackendActorRef actor, List<Message> parts, SendFlags flags) { return false; }
        @Override public void replyActorNoBind(ZLinkBackendActorRef actor, RoutingId sourceNodeRid, RoutingId sourceSessionRid, long requestId, int flags, List<Message> parts) { throw new UnsupportedOperationException(); }
        @Override public boolean sendToActor(ZLinkBackendActorRef actor, List<Message> parts, SendFlags flags) { return false; }
        @Override public java.util.concurrent.CompletionStage<List<Message>> requestToActor(ZLinkBackendActorRef actor, List<Message> parts, SendFlags flags, Duration timeout) { throw new UnsupportedOperationException(); }
        @Override public boolean forwardActorBoundSession(ZLinkBackendActorRef actor, RoutingId sourceNodeRid, RoutingId sourceSessionRid, List<Message> parts, SendFlags flags) { return false; }
        @Override public void bindRemoteActorBoundSession(ZLinkBackendActorRef actor, RoutingId sourceNodeRid, RoutingId sourceSessionRid) { }
        @Override public void closeActorBoundSession(ZLinkBackendActorRef actor, Duration timeout) { }
        @Override public String name() { return "fake-node"; }
        @Override public void close() { }
    }

    private static final class FakeSpot implements ZLinkBackendSpot {
        int sendAttempts;
        int sendFailuresRemaining;
        SendFlags lastSendFlags;
        int requestAttempts;
        int requestFailuresRemaining;
        ZLinkBackendRequestResult requestResult = ZLinkBackendRequestResult.OK;
        final ArrayDeque<ZLinkBackendRequestResult> requestResults =
            new ArrayDeque<>();
        SendFlags lastRequestFlags;
        long lastSpotGeneration;
        List<Message> requestReplyParts = List.of();

        @Override public String spotId() { return "entry-spot"; }
        @Override public void setRoutingId(String spotId) { }
        @Override public void setSubscription(String topic) { }
        @Override public ZLinkBackendTopicMessage subscribe(ZLinkBackendRecvMode mode) { return null; }
        @Override public ZLinkBackendReceived recvRoute(ZLinkBackendRecvMode mode) { return null; }
        @Override public boolean publish(String channelName, String topic, List<Message> parts, SendFlags flags) { return true; }
        @Override public boolean sendToSpot(RoutingId targetNodeRid, String spotId, long spotGeneration, List<Message> parts, SendFlags flags) {
            lastSpotGeneration = spotGeneration;
            sendAttempts++;
            lastSendFlags = flags;
            if (sendFailuresRemaining > 0) {
                sendFailuresRemaining--;
                return false;
            }
            return true;
        }
        @Override public boolean requestToSpot(
            RoutingId targetNodeRid,
            String spotId,
            long spotGeneration,
            List<Message> parts,
            ZLinkBackendRequestCallback callback,
            SendFlags flags,
            Duration timeout) {
            lastSpotGeneration = spotGeneration;
            requestAttempts++;
            lastRequestFlags = flags;
            if (requestFailuresRemaining > 0) {
                requestFailuresRemaining--;
                return false;
            }
            ZLinkBackendRequestResult result = requestResults.isEmpty()
                ? requestResult
                : requestResults.removeFirst();
            callback.handle(new ZLinkBackendReceived(
                result,
                Optional.empty(),
                Optional.of(spotId),
                Optional.empty(),
                requestReplyParts.stream().map(Message::from).toList()));
            return true;
        }
        @Override public void onDispatchEvent(ZLinkBackendSpotDispatchHandler handler) { }
        @Override public ZLinkBackendActorJoinRequest recvActorJoin(ZLinkBackendRecvMode mode) { return null; }
        @Override public void replyActorJoin(ZLinkBackendActorJoinRequest request, int joinResultCode, List<Message> parts) { }
        @Override public ZLinkBackendActorLifecycleEvent recvActorLifecycle(ZLinkBackendRecvMode mode) { return null; }
        @Override public String name() { return "fake-spot"; }
        @Override public void close() { requestReplyParts.forEach(Message::close); }
    }
}
