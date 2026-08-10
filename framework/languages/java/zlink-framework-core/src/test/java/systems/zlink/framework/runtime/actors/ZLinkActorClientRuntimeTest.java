package systems.zlink.framework.runtime.actors;
import java.lang.reflect.Proxy;
import java.util.EnumSet;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.host.ZLinkTestAdmissionFactory;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocation;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocationState;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.runtime.messaging.OneWayTestStatus;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

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
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
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
    void actorSendUsesDeclaredTypeForPayloadAndStreamCodec() {
        ZLinkCodecRegistration codecs = new ZLinkCodecRegistration();
        codecs.addSerializer(
            "application/x-broad",
            new MarkerSerializer("BROAD"),
            type -> type == BaseActorMessage.class || type == DerivedActorMessage.class);
        codecs.addStreamCodec("application/x-broad", ZLinkStreamCodec.MESSAGE_PACK);
        codecs.addSerializer(
            "application/x-base",
            new MarkerSerializer("BASE"),
            BaseActorMessage.class::equals);
        codecs.addStreamCodec("application/x-base", ZLinkStreamCodec.PROTOBUF);
        codecs.freeze();
        RecordingSpotNode node = new RecordingSpotNode();
        ZLinkActorClientRuntime client = new ZLinkActorClientRuntime(
            () -> node,
            new ZLinkStoreLocationResolvers(
                ZLinkRegisteredLocationStores.fromUnified(storeWithActor("actor-1")),
                new ZLinkLocationOptions()),
            codecs.serializerWithFallback(new ZLinkJsonMessageSerializer()),
            Duration.ofSeconds(5),
            ZLinkTestAdmissionFactory.create());

        client.sendToActor(
                "actor-1",
                ZLinkMessage.of(new DerivedActorMessage(), BaseActorMessage.class))
            .submit()
            .toCompletableFuture()
            .join();

        assertEquals(ZLinkStreamCodec.PROTOBUF, node.sentCodec);
        assertEquals("BaseActorMessage", node.sentPacketName);
        assertEquals("BASE", node.sentPayload);
    }

    @Test
    void actorReplyUsesTheSerializerMappedByItsWireCodec() {
        ZLinkCodecRegistration codecs = new ZLinkCodecRegistration();
        codecs.addSerializer(
            "application/x-reply",
            new ReplyMarkerSerializer("CUSTOM"),
            Pong.class::equals);
        codecs.addStreamCodec(
            "application/x-reply", ZLinkStreamCodec.PROTOBUF);
        codecs.freeze();
        RecordingSpotNode node = new RecordingSpotNode(
            streamReply(ZLinkStreamCodec.PROTOBUF, "wire"));
        ZLinkActorClientRuntime client = new ZLinkActorClientRuntime(
            () -> node,
            new ZLinkStoreLocationResolvers(
                ZLinkRegisteredLocationStores.fromUnified(storeWithActor("actor-1")),
                new ZLinkLocationOptions()),
            codecs.serializerWithFallback(new ZLinkJsonMessageSerializer()),
            Duration.ofSeconds(5),
            ZLinkTestAdmissionFactory.create());

        Pong reply = client.requestToActor("actor-1", new Ping("hello"))
            .submit(Pong.class)
            .toCompletableFuture()
            .join();

        assertEquals("CUSTOM", reply.value());
    }

    @Test
    void actorReplyWithUnmappedWireCodecIsProtocolError() {
        ZLinkActorClientRuntime client = new ZLinkActorClientRuntime(
            () -> new RecordingSpotNode(
                streamReply(ZLinkStreamCodec.PROTOBUF, "wire")),
            new ZLinkStoreLocationResolvers(
                ZLinkRegisteredLocationStores.fromUnified(storeWithActor("actor-1")),
                new ZLinkLocationOptions()),
            new ZLinkJsonMessageSerializer(),
            Duration.ofSeconds(5),
            ZLinkTestAdmissionFactory.create());

        CompletionException failure = assertThrows(
            CompletionException.class,
            () -> client.requestToActor("actor-1", new Ping("hello"))
                .submit(Pong.class)
                .toCompletableFuture()
                .join());

        ZLinkFrameworkException protocolError =
            (ZLinkFrameworkException) failure.getCause();
        assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, protocolError.kind());
    }

    @Test
    void sendAndRequestResolveGlobalActorIdAndUseBackendNoBindOperations() {
        ZLinkLocationRepository store =
            storeWithActor("actor-1");
        RecordingSpotNode node = new RecordingSpotNode(reply("pong"));
        ZLinkActorClientRuntime client = new ZLinkActorClientRuntime(
            () -> node,
            new ZLinkStoreLocationResolvers(
                ZLinkRegisteredLocationStores.fromUnified(store),
                new ZLinkLocationOptions()),
            new ZLinkJsonMessageSerializer(),
            Duration.ofSeconds(5),
            ZLinkTestAdmissionFactory.create());

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
                new ZLinkLocationOptions()),
            new ZLinkJsonMessageSerializer(),
            Duration.ofSeconds(5),
            ZLinkTestAdmissionFactory.create());
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
            Map.of("probe", ProbeActorFactory.class),
            Duration.ofSeconds(5),
            new ZLinkJsonMessageSerializer());

        assertEquals(
            Optional.empty(),
            actors.find("missing").toCompletableFuture().join());
    }

    @Test
    void actorManagerFindReturnsEmptyWhenMeshLookupReturnsNoActor() {
        ZLinkActorRuntime actors = new ZLinkActorRuntime(
            new EmptyLookupSpotNode(),
            Map.of("probe", ProbeActorFactory.class),
            Duration.ofSeconds(5),
            new ZLinkJsonMessageSerializer());

        assertEquals(
            Optional.empty(),
            actors.find("missing").toCompletableFuture().join());
    }

    @Test
    void explicitActorSendReturnsRouteNotConnectedWithoutPolling() {
        ZLinkLocationRepository store =
            storeWithActor("actor-1");
        RecordingSpotNode node = new RecordingSpotNode();
        node.sendFailure = SubmitResult.NOT_CONNECTED;
        ZLinkActorClientRuntime client = new ZLinkActorClientRuntime(
            () -> node,
            new ZLinkStoreLocationResolvers(
                ZLinkRegisteredLocationStores.fromUnified(store),
                new ZLinkLocationOptions()),
            new ZLinkJsonMessageSerializer(),
            Duration.ofMillis(250),
            ZLinkTestAdmissionFactory.create());

        var result = client.sendToActor("actor-1", new Ping("hello"))
            .submit();

        assertEquals(3,
            OneWayTestStatus.status(result));
        assertEquals(1, node.sendAttempts);
    }

    @Test
    void actorSendWaitsForFrameworkStartupBeforeTransportAdmission() {
        CompletableFuture<Void> runtimeReady =
            new CompletableFuture<>();
        RecordingSpotNode node = new RecordingSpotNode();
        ZLinkActorClientRuntime client = new ZLinkActorClientRuntime(
            () -> node,
            new ZLinkStoreLocationResolvers(
                ZLinkRegisteredLocationStores.fromUnified(storeWithActor("actor-1")),
                new ZLinkLocationOptions()),
            new ZLinkJsonMessageSerializer(),
            Duration.ofSeconds(5),
            ZLinkTestAdmissionFactory.create(),
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
            Map.entry(SubmitResult.NOT_FOUND, 4),
            Map.entry(SubmitResult.TERMINATED, 5))) {
            RecordingSpotNode node = new RecordingSpotNode();
            node.sendFailure = expected.getKey();
            ZLinkActorClientRuntime client = new ZLinkActorClientRuntime(
                () -> node,
                new ZLinkStoreLocationResolvers(
                    ZLinkRegisteredLocationStores.fromUnified(storeWithActor("actor-1")),
                    new ZLinkLocationOptions()),
                new ZLinkJsonMessageSerializer(),
                Duration.ofSeconds(5),
                ZLinkTestAdmissionFactory.create());

            var result = client.sendToActor("actor-1", new Ping("hello"))
                .submit();

            assertEquals(expected.getValue(),
                OneWayTestStatus.status(result));
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
                new ZLinkLocationOptions()),
            new ZLinkJsonMessageSerializer(),
            Duration.ofSeconds(5),
            ZLinkTestAdmissionFactory.create());

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
                new ZLinkLocationOptions()),
            new ZLinkJsonMessageSerializer(),
            Duration.ofSeconds(5),
            ZLinkTestAdmissionFactory.create());

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
                new ZLinkLocationOptions()),
            new ZLinkJsonMessageSerializer(),
            Duration.ofSeconds(5),
            ZLinkTestAdmissionFactory.create());

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
                new ZLinkLocationOptions()),
            new ZLinkJsonMessageSerializer(),
            Duration.ofSeconds(5),
            ZLinkTestAdmissionFactory.create());

        CompletionException error = assertThrows(
            CompletionException.class,
            () -> client.requestToActor("actor-1", new Ping("hello"))
                .submit(Pong.class)
                .toCompletableFuture()
                .join());

        ZLinkFrameworkException frameworkError = (ZLinkFrameworkException) error.getCause();
        assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, frameworkError.kind());
    }

    private static ZLinkLocationRepository
        storeWithActor(String actorId) {
        return storeWithActor(actorId, 7);
    }

    private static ZLinkLocationRepository
        storeWithActor(String actorId, long generation) {
        var codec = new systems.zlink.framework.runtime.locations
            .ZLinkActorAuthorityPayloadCodec();
        var snapshot = new ZLinkAuthoritySnapshot(
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
            new ZLinkPlacementAllocation(
                ZLinkPlacementAllocationState.ACTIVE,
                ZLinkPlacementObjectKind.ACTOR,
                "test",
                new ZLinkMeshNodeDescriptorKey(
                    "mesh", RoutingId.from("actor-node")),
                3,
                ZLinkPlacementCapacityBundle.actor(1)),
            Instant.now());
        return (ZLinkLocationRepository)
            Proxy.newProxyInstance(
                ZLinkActorClientRuntimeTest.class.getClassLoader(),
                new Class<?>[] {
                    ZLinkLocationRepository.class
                },
                (proxy, method, arguments) -> switch (method.getName()) {
                    case "read" -> CompletableFuture
                        .completedFuture(snapshot);
                    case "readOwnerLease" -> CompletableFuture
                        .completedFuture(
                            new ZLinkOwnerLeaseFound(
                                new ZLinkLocationOwnerToken("owner", 1),
                                Instant.now().plusSeconds(60),
                                Instant.now()));
                    default -> throw new UnsupportedOperationException(
                        method.getName());
                });
    }

    private static List<Message> reply(String value) {
        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.RESPONSE,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.of(1L),
            "Pong",
            Map.of());
        return List.of(
            Message.from(ZLinkStreamHeaderCodec.encode(header)),
            Message.from(serializer.serialize(new Pong(value)).bytes()));
    }

    private static List<Message> streamReply(
        ZLinkStreamCodec codec,
        String value) {
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.RESPONSE,
            codec,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.of(1L),
            "Pong",
            Map.of());
        return List.of(
            Message.from(ZLinkStreamHeaderCodec.encode(header)),
            Message.from(value.getBytes(StandardCharsets.UTF_8)));
    }

    private static List<Message> errorReply(String body) {
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.ERROR,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.of(1L),
            "",
            Map.of());
        return List.of(
            Message.from(ZLinkStreamHeaderCodec.encode(header)),
            Message.from(body.getBytes(StandardCharsets.UTF_8)));
    }

    private record Ping(String value) {
    }

    private record Pong(String value) {
    }

    private static class BaseActorMessage {
    }

    private static final class DerivedActorMessage extends BaseActorMessage {
    }

    private record MarkerSerializer(String marker) implements ZLinkMessageSerializer {
        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            return ZLinkEncodedPayload.from(marker.getBytes(StandardCharsets.UTF_8));
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            throw new UnsupportedOperationException();
        }
    }

    private record ReplyMarkerSerializer(String marker)
        implements ZLinkMessageSerializer {
        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            return ZLinkEncodedPayload.from(marker.getBytes(StandardCharsets.UTF_8));
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            return type.cast(new Pong(marker));
        }
    }

    public static final class ProbeActor implements ZLinkActor {
        private final String actorId;

        ProbeActor(String actorId) {
            this.actorId = actorId;
        }

        @Override
        public ZLinkActorContext context() {
            return null;
        }
    }

    public static final class ProbeActorFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(
            ZLinkActorContext context) {
            return CompletableFuture.completedFuture(
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
        ZLinkTestAdmissionFactory.Backend {
        private final List<Message> reply;
        ZLinkBackendActorRef sentActor;
        ZLinkBackendActorRef requestedActor;
        Map<String, String> sentMetadata = Map.of();
        Map<String, String> requestedMetadata = Map.of();
        ZLinkStreamCodec sentCodec;
        String sentPacketName;
        String sentPayload;
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
            ZLinkStreamHeader header =
                ZLinkStreamHeaderCodec.decodeOrPlain(parts.get(0).data());
            sentMetadata = header.metadata();
            sentCodec = header.codec();
            sentPacketName = header.packetName();
            sentPayload = parts.get(1).toUtf8String();
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
            return CompletableFuture.completedFuture(
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
            return CompletableFuture.failedFuture(new ZlinkRequestException(result));
        }
    }
}
