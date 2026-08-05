package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.time.Duration;
import java.time.Instant;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.contracts.errors.ZlinkConfigException;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;

import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinEntrySpotResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotRouteBridge;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkRegisteredLocationStores;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.spots.ZLinkSpotKind;

final class ZLinkActorClientRuntimeTest {
    @Test
    void sendAndRequestResolveGlobalActorIdAndUseBackendNoBindOperations() {
        systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository store =
            storeWithActor("actor-1");
        RecordingSpotNode node = new RecordingSpotNode(reply("pong"));
        ZLinkActorClientRuntime client = new ZLinkActorClientRuntime(
            () -> node,
            new ZLinkStoreLocationResolvers(
                ZLinkRegisteredLocationStores.fromUnified(store),
                new systems.zlink.framework.locations.ZLinkLocationOptions()),
            new ZLinkJsonMessageSerializer(),
            Duration.ofSeconds(5),
            systems.zlink.framework.runtime.host.ZLinkTestAdmissionFactory.create());

        client.sendToActor("actor-1", new Ping("hello"))
            .metadata("trace-id", "send-1")
            .submit();
        Pong pong = client.requestToActor("actor-1", new Ping("hello"))
            .metadata("trace-id", "request-1")
            .submit(Pong.class)
            .toCompletableFuture()
            .join();

        assertEquals("actor-1", node.sentActor.actorId());
        assertEquals(7, node.sentActor.generation());
        assertEquals("actor-1", node.requestedActor.actorId());
        assertEquals("send-1", node.sentMetadata.get("trace-id"));
        assertEquals("request-1", node.requestedMetadata.get("trace-id"));
        assertEquals("pong", pong.value());
    }

    @Test
    void globalActorIdRequiresCanonicalAuthorityResolution() {
        RecordingSpotNode node = new RecordingSpotNode(reply("pong"));
        ZLinkActorClientRuntime client = new ZLinkActorClientRuntime(
            () -> node,
            new ZLinkStoreLocationResolvers(
                ZLinkRegisteredLocationStores.fromUnified(storeWithActor("actor-direct", 17)),
                new systems.zlink.framework.locations.ZLinkLocationOptions()),
            new ZLinkJsonMessageSerializer(),
            Duration.ofSeconds(5),
            systems.zlink.framework.runtime.host.ZLinkTestAdmissionFactory.create());
        client.sendToActor("actor-direct", new Ping("hello"))
            .submit();
        Pong pong = client.requestToActor("actor-direct", new Ping("hello"))
            .submit(Pong.class)
            .toCompletableFuture()
            .join();

        assertEquals("actor-direct", node.sentActor.actorId());
        assertEquals(RoutingId.from("actor-node"), node.sentActor.nodeRid());
        assertEquals(17, node.sentActor.generation());
        assertEquals("actor-direct", node.requestedActor.actorId());
        assertEquals("pong", pong.value());
    }

    @Test
    void actorManagerFindReturnsEmptyWhenNativeLookupReportsNotFound() {
        ZLinkActorRuntime actors = new ZLinkActorRuntime(
            new MissingLookupSpotNode(),
            java.util.Map.of("probe", ProbeActorFactory.class),
            Duration.ofSeconds(5),
            new ZLinkJsonMessageSerializer());

        assertEquals(
            java.util.Optional.empty(),
            actors.find("missing").toCompletableFuture().join());
    }

    @Test
    void actorManagerFindReturnsEmptyWhenMeshLookupReturnsNoActor() {
        ZLinkActorRuntime actors = new ZLinkActorRuntime(
            new EmptyLookupSpotNode(),
            java.util.Map.of("probe", ProbeActorFactory.class),
            Duration.ofSeconds(5),
            new ZLinkJsonMessageSerializer());

        assertEquals(
            java.util.Optional.empty(),
            actors.find("missing").toCompletableFuture().join());
    }

    @Test
    void explicitActorSendReturnsRouteNotConnectedWithoutPolling() {
        systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository store =
            storeWithActor("actor-1");
        RecordingSpotNode node = new RecordingSpotNode();
        node.sendFailure = SubmitResult.NOT_CONNECTED;
        ZLinkActorClientRuntime client = new ZLinkActorClientRuntime(
            () -> node,
            new ZLinkStoreLocationResolvers(
                ZLinkRegisteredLocationStores.fromUnified(store),
                new systems.zlink.framework.locations.ZLinkLocationOptions()),
            new ZLinkJsonMessageSerializer(),
            Duration.ofMillis(250),
            systems.zlink.framework.runtime.host.ZLinkTestAdmissionFactory.create());

        var result = client.sendToActor("actor-1", new Ping("hello"))
            .submit();

        assertEquals(3,
            systems.zlink.framework.runtime.messaging.OneWayTestStatus.status(result));
        assertEquals(1, node.sendAttempts);
    }

    @Test
    void actorSendWaitsForFrameworkStartupBeforeTransportAdmission() {
        java.util.concurrent.CompletableFuture<Void> runtimeReady =
            new java.util.concurrent.CompletableFuture<>();
        RecordingSpotNode node = new RecordingSpotNode();
        ZLinkActorClientRuntime client = new ZLinkActorClientRuntime(
            () -> node,
            new ZLinkStoreLocationResolvers(
                ZLinkRegisteredLocationStores.fromUnified(storeWithActor("actor-1")),
                new systems.zlink.framework.locations.ZLinkLocationOptions()),
            new ZLinkJsonMessageSerializer(),
            Duration.ofSeconds(5),
            systems.zlink.framework.runtime.host.ZLinkTestAdmissionFactory.create(),
            runtimeReady);

        CompletionStage<Void> result = client.sendToActor(
            "actor-1", new Ping("hello")).submit();

        assertEquals(0, node.sendAttempts);
        runtimeReady.complete(null);
        result.toCompletableFuture().join();
        assertEquals(1, node.sendAttempts);
    }

    @Test
    void explicitActorSendPreservesTargetNotFoundAndShutdownResults() {
        for (var expected : List.of(
            java.util.Map.entry(SubmitResult.NOT_FOUND, 4),
            java.util.Map.entry(SubmitResult.TERMINATED, 5))) {
            RecordingSpotNode node = new RecordingSpotNode();
            node.sendFailure = expected.getKey();
            ZLinkActorClientRuntime client = new ZLinkActorClientRuntime(
                () -> node,
                new ZLinkStoreLocationResolvers(
                    ZLinkRegisteredLocationStores.fromUnified(storeWithActor("actor-1")),
                    new systems.zlink.framework.locations.ZLinkLocationOptions()),
                new ZLinkJsonMessageSerializer(),
                Duration.ofSeconds(5),
                systems.zlink.framework.runtime.host.ZLinkTestAdmissionFactory.create());

            var result = client.sendToActor("actor-1", new Ping("hello"))
                .submit();

            assertEquals(expected.getValue(),
                systems.zlink.framework.runtime.messaging.OneWayTestStatus.status(result));
            assertEquals(1, node.sendAttempts);
        }
    }

    @Test
    void staleRequestInvalidatesCacheWithoutRetryingTheSameOperation() {
        RequestFailingSpotNode node =
            new RequestFailingSpotNode(RequestResult.CONFLICT);
        ZLinkActorClientRuntime client = new ZLinkActorClientRuntime(
            () -> node,
            new ZLinkStoreLocationResolvers(
                ZLinkRegisteredLocationStores.fromUnified(storeWithActor("actor-1")),
                new systems.zlink.framework.locations.ZLinkLocationOptions()),
            new ZLinkJsonMessageSerializer(),
            Duration.ofSeconds(5),
            systems.zlink.framework.runtime.host.ZLinkTestAdmissionFactory.create());

        CompletionException error = assertThrows(
            CompletionException.class,
            () -> client.requestToActor("actor-1", new Ping("hello"))
                .submit(Pong.class)
                .toCompletableFuture()
                .join());

        ZLinkFrameworkException frameworkError = (ZLinkFrameworkException) error.getCause();
        assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, frameworkError.kind());
        assertEquals(1, node.requestAttempts);
    }

    @Test
    void inactiveExplicitActorRefMapsNotFoundToActorRouteNotFound() {
        ZLinkActorClientRuntime client = new ZLinkActorClientRuntime(
            () -> new RequestFailingSpotNode(RequestResult.NOT_FOUND),
            new ZLinkStoreLocationResolvers(
                ZLinkRegisteredLocationStores.fromUnified(storeWithActor("actor-1")),
                new systems.zlink.framework.locations.ZLinkLocationOptions()),
            new ZLinkJsonMessageSerializer(),
            Duration.ofSeconds(5),
            systems.zlink.framework.runtime.host.ZLinkTestAdmissionFactory.create());

        CompletionException error = assertThrows(
            CompletionException.class,
            () -> client.requestToActor("actor-1", new Ping("hello"))
                .submit(Pong.class)
                .toCompletableFuture()
                .join());

        ZLinkFrameworkException frameworkError = (ZLinkFrameworkException) error.getCause();
        assertEquals(ZLinkFrameworkErrorKind.NOT_FOUND, frameworkError.kind());
    }

    @Test
    void actorRequestDecodesFrameworkErrorEnvelope() {
        ZLinkActorClientRuntime client = new ZLinkActorClientRuntime(
            () -> new RecordingSpotNode(errorReply(
                "{\"code\":\"NotFound\",\"message\":\"remote actor is missing\"}")),
            new ZLinkStoreLocationResolvers(
                ZLinkRegisteredLocationStores.fromUnified(storeWithActor("actor-1")),
                new systems.zlink.framework.locations.ZLinkLocationOptions()),
            new ZLinkJsonMessageSerializer(),
            Duration.ofSeconds(5),
            systems.zlink.framework.runtime.host.ZLinkTestAdmissionFactory.create());

        CompletionException error = assertThrows(
            CompletionException.class,
            () -> client.requestToActor("actor-1", new Ping("hello"))
                .submit(Pong.class)
                .toCompletableFuture()
                .join());

        ZLinkFrameworkException frameworkError = (ZLinkFrameworkException) error.getCause();
        assertEquals(ZLinkFrameworkErrorKind.NOT_FOUND, frameworkError.kind());
        assertEquals("remote actor is missing", frameworkError.getMessage());
    }

    @Test
    void actorRequestMapsMalformedErrorEnvelopeToProtocolError() {
        ZLinkActorClientRuntime client = new ZLinkActorClientRuntime(
            () -> new RecordingSpotNode(errorReply("not-json")),
            new ZLinkStoreLocationResolvers(
                ZLinkRegisteredLocationStores.fromUnified(storeWithActor("actor-1")),
                new systems.zlink.framework.locations.ZLinkLocationOptions()),
            new ZLinkJsonMessageSerializer(),
            Duration.ofSeconds(5),
            systems.zlink.framework.runtime.host.ZLinkTestAdmissionFactory.create());

        CompletionException error = assertThrows(
            CompletionException.class,
            () -> client.requestToActor("actor-1", new Ping("hello"))
                .submit(Pong.class)
                .toCompletableFuture()
                .join());

        ZLinkFrameworkException frameworkError = (ZLinkFrameworkException) error.getCause();
        assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, frameworkError.kind());
    }

    private static systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository
        storeWithActor(String actorId) {
        return storeWithActor(actorId, 7);
    }

    private static systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository
        storeWithActor(String actorId, long generation) {
        var codec = new systems.zlink.framework.runtime.locations
            .ZLinkActorAuthorityPayloadCodec();
        var snapshot = new systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot(
            "v1",
            codec.encode(
                systems.zlink.framework.runtime.locations
                    .ZLinkActorAuthorityPayloadCodec.State.READY,
                "test",
                actorId,
                "entry-spot",
                1,
                1,
                "owner",
                1,
                "mesh",
                RoutingId.from("actor-node"),
                3),
            generation,
            5,
            "owner",
            1,
            new systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocation(
                systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocationState.ACTIVE,
                systems.zlink.framework.locations.ZLinkPlacementObjectKind.ACTOR,
                "test",
                new systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey(
                    "mesh", RoutingId.from("actor-node")),
                3,
                systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle.actor(1)),
            Instant.now());
        return (systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository)
            java.lang.reflect.Proxy.newProxyInstance(
                ZLinkActorClientRuntimeTest.class.getClassLoader(),
                new Class<?>[] {
                    systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository.class
                },
                (proxy, method, arguments) -> switch (method.getName()) {
                    case "read" -> java.util.concurrent.CompletableFuture
                        .completedFuture(snapshot);
                    case "readOwnerLease" -> java.util.concurrent.CompletableFuture
                        .completedFuture(
                            new systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseFound(
                                new systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken("owner", 1),
                                Instant.now().plusSeconds(60),
                                Instant.now()));
                    default -> throw new UnsupportedOperationException(
                        method.getName());
                });
    }

    private static List<Message> reply(String value) {
        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            systems.zlink.framework.streams.ZLinkStreamMessageKind.RESPONSE,
            systems.zlink.framework.streams.ZLinkStreamCodec.JSON,
            java.util.EnumSet.noneOf(systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag.class),
            java.util.Optional.of(1L),
            "Pong",
            java.util.Map.of());
        return List.of(
            Message.from(ZLinkStreamHeaderCodec.encode(header)),
            Message.from(serializer.serialize(new Pong(value)).bytes()));
    }

    private static List<Message> errorReply(String body) {
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            systems.zlink.framework.streams.ZLinkStreamMessageKind.ERROR,
            systems.zlink.framework.streams.ZLinkStreamCodec.JSON,
            java.util.EnumSet.noneOf(systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag.class),
            java.util.Optional.of(1L),
            "",
            java.util.Map.of());
        return List.of(
            Message.from(ZLinkStreamHeaderCodec.encode(header)),
            Message.from(body.getBytes(StandardCharsets.UTF_8)));
    }

    private record Ping(String value) {
    }

    private record Pong(String value) {
    }

    public static final class ProbeActor implements systems.zlink.framework.actors.ZLinkActor {
        private final String actorId;

        ProbeActor(String actorId) {
            this.actorId = actorId;
        }

        @Override
        public systems.zlink.framework.actors.ZLinkActorContext context() {
            return null;
        }
    }

    public static final class ProbeActorFactory implements systems.zlink.framework.actors.ZLinkActorFactory {
        @Override
        public CompletionStage<systems.zlink.framework.actors.ZLinkActor> create(
            systems.zlink.framework.actors.ZLinkActorContext context) {
            return java.util.concurrent.CompletableFuture.completedFuture(
                new ProbeActor(context.actorId()));
        }
    }

    private static final class MissingLookupSpotNode extends RecordingSpotNode {
        @Override
        public ZLinkBackendActorRef actorLookup(String actorId) {
            throw new ZlinkConfigException(ConfigResult.NOT_FOUND);
        }
    }

    private static final class EmptyLookupSpotNode extends RecordingSpotNode {
        @Override
        public ZLinkBackendActorRef actorLookup(String actorId) {
            return null;
        }
    }

    private static class RecordingSpotNode implements ZLinkInternalSpotNode,
        systems.zlink.framework.runtime.host.ZLinkTestAdmissionFactory.Backend {
        private final List<Message> reply;
        ZLinkBackendActorRef sentActor;
        ZLinkBackendActorRef requestedActor;
        java.util.Map<String, String> sentMetadata = java.util.Map.of();
        java.util.Map<String, String> requestedMetadata = java.util.Map.of();
        int sendAttempts;
        SubmitResult sendFailure;

        RecordingSpotNode() {
            this(reply("unused"));
        }

        RecordingSpotNode(List<Message> reply) {
            this.reply = reply;
        }

        @Override public RoutingId routingId() { return RoutingId.from("caller"); }
        @Override public void setRoutingId(RoutingId routingId) { }
        @Override public void setPublisherRoutingId(RoutingId routingId) { }
        @Override public void setSubscriberRoutingId(RoutingId routingId) { }
        @Override public void setRouterBind(String endpoint) { }
        @Override public void setPubBind(String endpoint) { }
        @Override public void connectPeer(String endpoint) { }
        @Override public void connectPeer(RoutingId peerRid, String endpoint) { }
        @Override public void disconnectPeer(String endpoint) { }
        @Override public void disconnectPeer(RoutingId peerRid) { }
        @Override public ZLinkBackendSpotRouteBridge createRouteBridge() { throw new UnsupportedOperationException(); }
        @Override public ZLinkBackendSpot createSpot() { throw new UnsupportedOperationException(); }
        @Override public ZLinkBackendSpot entrySpot() { throw new UnsupportedOperationException(); }
        @Override public ZLinkBackendActorRef createActor(String actorId, Message createRequest) { throw new UnsupportedOperationException(); }
        @Override public ZLinkBackendActorRef actorLookup(String actorId) { throw new UnsupportedOperationException(); }
        @Override public CompletionStage<ZLinkBackendActorJoinResult> joinActor(ZLinkBackendActorRef actor, RoutingId targetNodeRid, String targetSpotId, List<Message> parts, Duration timeout) { throw new UnsupportedOperationException(); }
        @Override public CompletionStage<ZLinkBackendActorJoinEntrySpotResult> joinActorEntrySpot(ZLinkBackendActorRef actor, RoutingId targetNodeRid, Message request, Duration timeout) { throw new UnsupportedOperationException(); }
        @Override public CompletionStage<List<Message>> leaveActor(ZLinkBackendActorRef actor, String currentSpotId, Duration timeout) { throw new UnsupportedOperationException(); }
        @Override public CompletionStage<Void> destroyActor(ZLinkBackendActorRef actor, Duration timeout) { throw new UnsupportedOperationException(); }
        @Override public boolean sendActorBoundSession(ZLinkBackendActorRef actor, List<Message> parts, SendFlags flags) { throw new UnsupportedOperationException(); }
        @Override public void replyActorNoBind(ZLinkBackendActorRef actor, RoutingId sourceNodeRid, RoutingId sourceSessionRid, long requestId, int flags, List<Message> parts) { throw new UnsupportedOperationException(); }

        @Override
        public boolean sendToActor(ZLinkBackendActorRef actor, List<Message> parts, SendFlags flags) {
            sendAttempts++;
            if (sendFailure != null) {
                throw new ZlinkSubmitException(sendFailure);
            }
            sentActor = actor;
            sentMetadata = ZLinkStreamHeaderCodec.decodeOrPlain(parts.get(0).data())
                .metadata();
            return true;
        }

        @Override
        public CompletionStage<List<Message>> requestToActor(
            ZLinkBackendActorRef actor,
            List<Message> parts,
            SendFlags flags,
            Duration timeout) {
            requestedActor = actor;
            requestedMetadata = ZLinkStreamHeaderCodec.decodeOrPlain(parts.get(0).data())
                .metadata();
            return java.util.concurrent.CompletableFuture.completedFuture(
                reply.stream().map(Message::from).toList());
        }

        @Override public boolean forwardActorBoundSession(ZLinkBackendActorRef actor, RoutingId sourceNodeRid, RoutingId sourceSessionRid, List<Message> parts, SendFlags flags) { throw new UnsupportedOperationException(); }
        @Override public void bindRemoteActorBoundSession(ZLinkBackendActorRef actor, RoutingId sourceNodeRid, RoutingId sourceSessionRid) { }
        @Override public void closeActorBoundSession(ZLinkBackendActorRef actor, Duration timeout) { }
        @Override public String name() { return "recording"; }
        @Override public void close() { }
    }

    private static final class RequestFailingSpotNode extends RecordingSpotNode {
        private final RequestResult result;
        private int requestAttempts;

        RequestFailingSpotNode(RequestResult result) {
            this.result = result;
        }

        @Override
        public CompletionStage<List<Message>> requestToActor(
            ZLinkBackendActorRef actor,
            List<Message> parts,
            SendFlags flags,
            Duration timeout) {
            requestAttempts++;
            return java.util.concurrent.CompletableFuture.failedFuture(new ZlinkRequestException(result));
        }
    }
}
