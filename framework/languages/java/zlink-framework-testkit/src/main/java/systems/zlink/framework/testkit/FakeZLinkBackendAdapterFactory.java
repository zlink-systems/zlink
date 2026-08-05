package systems.zlink.framework.testkit;

import java.time.Duration;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Deque;
import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Queue;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;
import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.contracts.errors.ZlinkConfigException;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry;
import systems.zlink.framework.runtime.internal.binding.spot.ActorTransferPrepare;
import systems.zlink.framework.runtime.internal.binding.spot.ActorTransferPrepareResult;
import systems.zlink.framework.runtime.internal.binding.spot.ActorTransferToken;
import systems.zlink.framework.testkit.internal.ActorTransferTokenFixture;
import systems.zlink.framework.runtime.internal.binding.spot.PrepareActorTransferResult;
import systems.zlink.framework.runtime.internal.binding.spot.OwnerKind;
import systems.zlink.framework.runtime.internal.binding.spot.ReadyRecord;
import systems.zlink.framework.runtime.internal.binding.spot.ReceiveRecord;
import systems.zlink.framework.runtime.internal.binding.spot.RecordKind;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorBindOperation;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinEntrySpotResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinRequest;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorLifecycleEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorLifecycleEventKind;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorLifecycleInfo;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRoute;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorUnbindOperation;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendPublisherSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestCallback;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitorHandler;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitorEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchHandler;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchInfo;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotNodeMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotRoute;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotRouteBridge;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamErrorHandler;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSubscriberSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkMonitoringBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkSpotBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkStreamBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;
import systems.zlink.framework.spots.ZLinkSpotKind;

public final class FakeZLinkBackendAdapterFactory implements ZLinkBackendAdapterProvider {
    private final List<String> calls = Collections.synchronizedList(new ArrayList<>());
    private final List<FakeStreamSocket> streams = new ArrayList<>();
    private final List<FakeSpot> spots = new ArrayList<>();
    private final List<FakeRouterSocket> routers = new ArrayList<>();
    private final List<FakeMeshNode> meshNodes = new ArrayList<>();
    private final Map<String, ZLinkBackendActorRef> actors = new ConcurrentHashMap<>();
    private Message nextActorJoinReply;
    private List<Message> nextSpotRequestReplyParts;
    private volatile Consumer<String> spotReplyObserver = ignored -> { };
    private volatile byte[] lastApplicationMetadata = new byte[0];

    private record FakeActorJoinReply(int resultCode, List<Message> parts) {
    }

    public List<String> calls() {
        synchronized (calls) {
            return List.copyOf(calls);
        }
    }

    public byte[] lastApplicationMetadata() {
        return lastApplicationMetadata.clone();
    }

    private void captureApplicationMetadata(byte[] metadata) {
        lastApplicationMetadata =
            metadata == null ? new byte[0] : metadata.clone();
    }

    public void nextActorJoinReply(Message reply) {
        if (nextActorJoinReply != null) {
            nextActorJoinReply.close();
        }
        nextActorJoinReply = Message.from(reply);
    }

    public void nextSpotRequestReplyParts(List<Message> parts) {
        if (nextSpotRequestReplyParts != null) {
            nextSpotRequestReplyParts.forEach(Message::close);
        }
        nextSpotRequestReplyParts = parts.stream()
            .map(Message::from)
            .toList();
    }

    public void onSpotReply(Consumer<String> observer) {
        spotReplyObserver = observer == null ? ignored -> { } : observer;
    }

    private static Message jsonStringMessage(String value) {
        return Message.from(("\"" + value + "\"").getBytes(StandardCharsets.UTF_8));
    }

    public void dispatchStreamPacket(String packetName, String payload) {
        if (streams.isEmpty()) {
            throw new IllegalStateException("no fake stream socket is available");
        }
        streams.get(0).dispatchPacket(
            RoutingId.from("fake-session"),
            Message.from(encodeStreamHeader(1, 0, packetName, Optional.empty())),
            jsonStringMessage(payload));
    }

    public void dispatchStreamPacket(
        String packetName,
        Message payload,
        ZLinkStreamCodec codec) {
        if (streams.isEmpty()) {
            throw new IllegalStateException("no fake stream socket is available");
        }
        streams.get(0).dispatchPacket(
            RoutingId.from("fake-session"),
            Message.from(encodeStreamHeader(1, codec.value(), packetName, Optional.empty())),
            payload);
    }

    public void dispatchStreamRequest(String packetName, String payload, long requestSeq) {
        dispatchStreamRequest(packetName, jsonStringMessage(payload), requestSeq, 0);
    }

    public void dispatchStreamRequest(
        String packetName,
        Message payload,
        long requestSeq,
        int flags) {
        if (streams.isEmpty()) {
            throw new IllegalStateException("no fake stream socket is available");
        }
        streams.get(0).dispatchPacket(
            RoutingId.from("fake-session"),
            Message.from(encodeStreamHeader(2, 0, packetName, Optional.of(requestSeq), flags)),
            payload);
    }

    public void dispatchStreamControl(String packetName) {
        if (streams.isEmpty()) {
            throw new IllegalStateException("no fake stream socket is available");
        }
        streams.get(0).dispatchPacket(
            RoutingId.from("fake-session"),
            Message.from(encodeStreamHeader(
                ZLinkStreamMessageKind.CONTROL.value(),
                ZLinkStreamCodec.RAW.value(),
                packetName,
                Optional.empty())),
            Message.from(new byte[0]));
    }

    public void dispatchStreamTransportError(int nativeCode, String message) {
        if (streams.isEmpty()) {
            throw new IllegalStateException("no fake stream socket is available");
        }
        streams.get(0).dispatchTransportError(
            RoutingId.from("fake-session"),
            nativeCode,
            message);
    }

    public void dispatchEntrySpotActorJoinReadable(String actorId) {
        dispatchEntrySpotActorJoinReadable(actorId, null, "join");
    }

    public void dispatchEntrySpotActorJoinReadable(String actorId, String packetName, String payload) {
        FakeSpot entrySpot = spots.stream()
            .filter(spot -> "entrySpot".equals(spot.name()))
            .findFirst()
            .orElseThrow(() -> new IllegalStateException("no fake entry spot is available"));
        entrySpot.enqueueActorJoin(actorId, packetName, payload);
        entrySpot.dispatchActorJoinReadable();
    }

    public void dispatchEntrySpotActorMessage(String actorId, String packetName, String payload) {
        FakeSpot entrySpot = spots.stream()
            .filter(spot -> "entrySpot".equals(spot.name()))
            .findFirst()
            .orElseThrow(() -> new IllegalStateException("no fake entry spot is available"));
        entrySpot.dispatchActorMessage(actorId, packetName, payload, Optional.empty());
    }

    public void dispatchEntrySpotActorRequest(
        String actorId,
        String packetName,
        String payload,
        long requestSeq) {
        FakeSpot entrySpot = spots.stream()
            .filter(spot -> "entrySpot".equals(spot.name()))
            .findFirst()
            .orElseThrow(() -> new IllegalStateException("no fake entry spot is available"));
        entrySpot.dispatchActorMessage(actorId, packetName, payload, Optional.of(requestSeq));
    }

    public void dispatchEntrySpotActorStreamRequest(
        String actorId,
        String packetName,
        String payload,
        long requestSeq) {
        FakeSpot entrySpot = spots.stream()
            .filter(spot -> "entrySpot".equals(spot.name()))
            .findFirst()
            .orElseThrow(() -> new IllegalStateException("no fake entry spot is available"));
        entrySpot.dispatchActorMessage(
            actorId,
            encodeStreamHeader(2, 0, packetName, Optional.of(requestSeq)),
            payload,
            Optional.empty());
    }

    public void dispatchEntrySpotActorLifecycleLeft(String actorId) {
        FakeSpot entrySpot = spots.stream()
            .filter(spot -> "entrySpot".equals(spot.name()))
            .findFirst()
            .orElseThrow(() -> new IllegalStateException("no fake entry spot is available"));
        entrySpot.enqueueActorLifecycleLeft(actorId);
        entrySpot.dispatchActorLifecycleReadable();
    }

    public void dispatchEntrySpotActorLifecycleJoined(String actorId, String spotId) {
        FakeSpot entrySpot = spots.stream()
            .filter(spot -> "entrySpot".equals(spot.name()))
            .findFirst()
            .orElseThrow(() -> new IllegalStateException("no fake entry spot is available"));
        entrySpot.enqueueActorLifecycleJoined(actorId, spotId);
        entrySpot.dispatchActorLifecycleReadable();
    }

    public void dispatchSpotActorLifecycleLeft(String actorId) {
        FakeSpot spot = firstUserSpot();
        spot.enqueueActorLifecycleLeft(actorId);
        spot.dispatchActorLifecycleReadable();
    }

    public void dispatchSpotRoute(String packetName, String payload) {
        dispatchSpotRoute(packetName, payload, new byte[0]);
    }

    public void dispatchSpotRoute(
        String packetName,
        String payload,
        byte[] metadata) {
        FakeSpot spot = firstUserSpot();
        spot.enqueueRoute(packetName, payload, Optional.empty(), metadata);
        spot.dispatchRouteReadable();
    }

    public void dispatchSpotRequest(String packetName, String payload, long requestSeq) {
        dispatchSpotRequest(packetName, payload, requestSeq, new byte[0]);
    }

    public void dispatchSpotRequest(
        String packetName,
        String payload,
        long requestSeq,
        byte[] metadata) {
        FakeSpot spot = firstUserSpot();
        spot.enqueueRoute(packetName, payload, Optional.of(requestSeq), metadata);
        spot.dispatchRouteReadable();
    }

    public void dispatchEntrySpotRequest(String packetName, String payload, long requestSeq) {
        FakeSpot spot = entrySpot();
        spot.enqueueRoute(packetName, payload, Optional.of(requestSeq));
        spot.dispatchRouteReadable();
    }

    public void dispatchRouteMeshSpotRequest(
        RoutingId sourceRid,
        String targetSpotId,
        List<Message> spotParts,
        long requestSeq) {
        if (routers.isEmpty()) {
            throw new IllegalStateException("no fake route socket is available");
        }
        routers.get(0).enqueueReceived(sourceRid, Optional.of(requestSeq), copyMessages(spotParts));
    }

    public void dispatchSpotSubscription(String topic, String packetName, String payload) {
        dispatchSpotSubscription(topic, packetName, payload, new byte[0]);
    }

    public void dispatchSpotSubscription(
        String topic,
        String packetName,
        String payload,
        byte[] metadata) {
        FakeSpot spot = firstUserSpot();
        spot.enqueueSubscription(topic, packetName, payload, metadata);
        spot.dispatchSubscribeReadable();
    }

    public void dispatchMeshNodeSend(String packetName, String jsonPayload) {
        dispatchMeshSend(RecordKind.NODE_SEND, null, packetName, jsonPayload);
    }

    public void dispatchMeshChannelSend(
        String channelName,
        String packetName,
        String jsonPayload) {
        dispatchMeshSend(RecordKind.CHANNEL_SEND, channelName, packetName, jsonPayload);
    }

    private void dispatchMeshSend(
        RecordKind kind,
        String channelName,
        String packetName,
        String jsonPayload) {
        if (meshNodes.isEmpty()) {
            throw new IllegalStateException("no fake MeshNode is available");
        }
        meshNodes.get(0).dispatch(new ZLinkMeshDispatchRecord(
            new ReadyRecord(OwnerKind.NODE, 1, null, null),
            new ReceiveRecord(
                kind,
                1,
                RoutingId.from("fake-mesh-source"),
                null,
                null,
                null,
                null,
                null,
                channelName,
                null,
                new byte[0],
                0,
                0,
                0,
                2),
            List.of(
                Message.from(packetName.getBytes(StandardCharsets.UTF_8)),
                Message.from(jsonPayload.getBytes(StandardCharsets.UTF_8)))));
    }

    public void dispatchSpotActorJoinReadable(String actorId, String packetName, String payload) {
        FakeSpot spot = firstUserSpot();
        spot.enqueueActorJoin(actorId, packetName, payload);
        spot.dispatchActorJoinReadable();
    }

    public void dispatchSpotActorStreamRequest(
        String actorId,
        String packetName,
        String payload,
        long requestSeq) {
        FakeSpot spot = firstUserSpot();
        spot.dispatchActorMessage(
            actorId,
            encodeStreamHeader(2, 0, packetName, Optional.of(requestSeq)),
            payload,
            Optional.empty());
    }

    public List<String> spotReplies() {
        return spots.stream()
            .flatMap(spot -> spot.replies().stream())
            .toList();
    }

    private FakeSpot firstUserSpot() {
        return spots.stream()
            .filter(spot -> spot.name().startsWith("spot."))
            .findFirst()
            .orElseThrow(() -> new IllegalStateException("no fake user spot is available"));
    }

    private FakeSpot entrySpot() {
        return spots.stream()
            .filter(spot -> "entrySpot".equals(spot.name()))
            .findFirst()
            .orElseThrow(() -> new IllegalStateException("no fake entry spot is available"));
    }

    @Override
    public ZLinkChannelBackendAdapter createChannelAdapter(ZLinkBackendAdapterOptions options) {
        calls.add("factory.channel");
        return new FakeChannelBackendAdapter(calls, routers, this);
    }

    @Override
    public ZLinkSpotBackendAdapter createSpotAdapter(ZLinkBackendAdapterOptions options) {
        calls.add("factory.spot");
        return new FakeSpotBackendAdapter(calls, spots, this);
    }

    @Override
    public ZLinkMeshBackendAdapter createMeshAdapter(ZLinkBackendAdapterOptions options) {
        calls.add("factory.mesh");
        return (context, meshName) -> {
            FakeMeshNode node =
                new FakeMeshNode(calls, meshName, spots, this);
            meshNodes.add(node);
            return node;
        };
    }

    @Override
    public ZLinkStreamBackendAdapter createStreamAdapter(ZLinkBackendAdapterOptions options) {
        calls.add("factory.stream");
        return new FakeStreamBackendAdapter(calls, streams);
    }

    private static byte[] encodeStreamHeader(
        int kind,
        int codec,
        String packetName,
        Optional<Long> requestSeq) {
        return encodeStreamHeader(kind, codec, packetName, requestSeq, 0);
    }

    private static byte[] encodeStreamHeader(
        int kind,
        int codec,
        String packetName,
        Optional<Long> requestSeq,
        int additionalFlags) {
        EnumSet<ZLinkStreamHeaderFlag> flags = EnumSet.noneOf(ZLinkStreamHeaderFlag.class);
        if ((additionalFlags & ZLinkStreamHeaderFlag.PAYLOAD_COMPRESSED.value()) != 0) {
            flags.add(ZLinkStreamHeaderFlag.PAYLOAD_COMPRESSED);
        }
        int supportedFlags = ZLinkStreamHeaderFlag.PAYLOAD_COMPRESSED.value();
        if ((additionalFlags & ~supportedFlags) != 0) {
            throw new IllegalArgumentException(
                "fake STREAM dispatch does not have values for flags: " + additionalFlags);
        }
        return ZLinkStreamHeaderCodec.encode(new ZLinkStreamHeader(
            ZLinkStreamMessageKind.fromValue(kind),
            ZLinkStreamCodec.fromValue(codec),
            flags,
            requestSeq,
            packetName,
            Map.of()));
    }

    @Override
    public ZLinkMonitoringBackendAdapter createMonitoringAdapter(ZLinkBackendAdapterOptions options) {
        calls.add("factory.monitoring");
        return new FakeMonitoringBackendAdapter(calls);
    }

    private abstract static class FakeBackendObject implements ZLinkBackendObject {
        private final List<String> calls;
        private final String name;

        FakeBackendObject(List<String> calls, String name) {
            this.calls = calls;
            this.name = name;
            calls.add("create." + name);
        }

        @Override
        public String name() {
            return name;
        }

        @Override
        public void close() {
            calls.add("close." + name);
        }

        void record(String call) {
            calls.add(name + "." + call);
        }

        List<String> calls() {
            return calls;
        }
    }

    private static final class FakeChannelBackendAdapter implements ZLinkChannelBackendAdapter {
        private final List<String> calls;
        private final List<FakeRouterSocket> routers;

        FakeChannelBackendAdapter(
            List<String> calls,
            List<FakeRouterSocket> routers,
            FakeZLinkBackendAdapterFactory owner) {
            this.calls = calls;
            this.routers = routers;
        }

        @Override
        public ZLinkBackendContext createContext() {
            return new FakeContext(calls);
        }

        @Override
        public ZLinkBackendDealerSocket createDealerSocket(ZLinkBackendContext context) {
            return new FakeDealerSocket(calls, "dealer");
        }

        @Override
        public ZLinkBackendRouterSocket createRouterSocket(ZLinkBackendContext context) {
            FakeRouterSocket router = new FakeRouterSocket(calls, "router");
            routers.add(router);
            return router;
        }

        @Override
        public ZLinkBackendPublisherSocket createPublisherSocket(ZLinkBackendContext context) {
            return new FakePublisherSocket(calls, "publisher");
        }

        @Override
        public ZLinkBackendSubscriberSocket createSubscriberSocket(ZLinkBackendContext context) {
            return new FakeSubscriberSocket(calls, "subscriber");
        }
    }

    private static final class FakeSpotBackendAdapter implements ZLinkSpotBackendAdapter {
        private final List<String> calls;
        private final List<FakeSpot> spots;
        private final FakeZLinkBackendAdapterFactory owner;

        FakeSpotBackendAdapter(
            List<String> calls,
            List<FakeSpot> spots,
            FakeZLinkBackendAdapterFactory owner) {
            this.calls = calls;
            this.spots = spots;
            this.owner = owner;
        }

        @Override
        public ZLinkInternalSpotNode createSpotNode(ZLinkBackendContext context, ZLinkBackendSpotNodeMode mode) {
            return new FakeSpotNode(calls, spots, owner);
        }
    }

    private static final class FakeStreamBackendAdapter implements ZLinkStreamBackendAdapter {
        private final List<String> calls;
        private final List<FakeStreamSocket> streams;

        FakeStreamBackendAdapter(List<String> calls, List<FakeStreamSocket> streams) {
            this.calls = calls;
            this.streams = streams;
        }

        @Override
        public ZLinkBackendStreamSocket createStreamSocket(
            ZLinkBackendContext context,
            systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode meshNode) {
            FakeStreamSocket stream = new FakeStreamSocket(calls);
            streams.add(stream);
            return stream;
        }
    }

    private static final class FakeMonitoringBackendAdapter implements ZLinkMonitoringBackendAdapter {
        private final List<String> calls;

        FakeMonitoringBackendAdapter(List<String> calls) {
            this.calls = calls;
        }

        @Override
        public ZLinkBackendSocketMonitor openSocketMonitor(ZLinkBackendSocket socket) {
            calls.add("monitoring.open." + socket.name());
            return new FakeSocketMonitor(calls);
        }
    }

    private static final class FakeContext extends FakeBackendObject implements ZLinkBackendContext {
        FakeContext(List<String> calls) {
            super(calls, "context");
        }

        @Override
        public void shutdown() {
            record("shutdown");
        }
    }

    private static final class FakeMeshNode
        extends FakeBackendObject
        implements ZLinkInternalMeshNode {
        private final String meshName;
        private RoutingId routingId = RoutingId.from("fake-mesh-node");
        private String endpoint = "";
        private MeshNodeState state = MeshNodeState.CREATED;
        private Consumer<ZLinkMeshDispatchRecord> receiver = ZLinkMeshDispatchRecord::close;
        private final List<Long> connectionIntents = new ArrayList<>();
        private final ZLinkInternalSpotNode spotNode;

        FakeMeshNode(
            List<String> calls,
            String name,
            List<FakeSpot> spots,
            FakeZLinkBackendAdapterFactory owner) {
            super(calls, "mesh." + name);
            meshName = name;
            spotNode = new FakeSpotNode(calls, spots, owner);
        }

        @Override public String name() { return meshName; }
        @Override public void setBind(String value) {
            endpoint = value;
            record("bind." + value);
        }
        @Override public void addChannel(String channelName) { record("channel." + channelName); }
        @Override public void setChannelWeight(String channelName, int weight) {
            record("weight." + channelName + "." + weight);
        }
        @Override public void setRoutingId(RoutingId value) { routingId = value; }
        @Override public void start() {
            state = MeshNodeState.STARTED;
            record("start");
        }
        @Override public long connectPeer(String endpoint) {
            return addConnection(endpoint);
        }
        @Override public long connectPeer(String endpoint, RoutingId expectedRoutingId) {
            return addConnection(endpoint);
        }
        @Override public MeshNodeStatus status() {
            return new MeshNodeStatus(
                state,
                routingId,
                meshName,
                endpoint,
                1,
                1,
                0,
                connectionIntents.size(),
                connectionIntents.size(),
                0,
                0,
                0,
                0,
                0,
                System.currentTimeMillis());
        }
        @Override public List<MeshPeerEntry> peers() { return List.of(); }
        @Override public List<Long> connectionIntentIds() {
            return List.copyOf(connectionIntents);
        }
        @Override public void startDispatch(Consumer<ZLinkMeshDispatchRecord> value) {
            receiver = value;
            record("dispatch");
        }
        @Override public ZLinkInternalSpotNode spotNode() {
            return spotNode;
        }

        void dispatch(ZLinkMeshDispatchRecord record) {
            receiver.accept(record);
        }

        private long addConnection(String endpoint) {
            long id = connectionIntents.size() + 1L;
            connectionIntents.add(id);
            record("connect." + endpoint);
            return id;
        }
    }

    private abstract static class FakeSocket extends FakeBackendObject implements ZLinkBackendSocket {
        FakeSocket(List<String> calls, String name) {
            super(calls, name);
        }

        @Override
        public void bind(String endpoint) {
            record("bind." + endpoint);
        }
    }

    private abstract static class FakeConnectableSocket extends FakeSocket {
        FakeConnectableSocket(List<String> calls, String name) {
            super(calls, name);
        }

        public void connect(String endpoint) {
            record("connect." + endpoint);
        }

        public void disconnect(String endpoint) {
            record("disconnect." + endpoint);
        }
    }

    private static final class FakeDealerSocket extends FakeConnectableSocket implements ZLinkBackendDealerSocket {
        FakeDealerSocket(List<String> calls, String name) {
            super(calls, name);
        }

        @Override public void setChannelName(String channelName) { record("setChannelName." + channelName); }
        @Override public boolean waitForReadable(Duration timeout) { return false; }
        @Override public boolean send(List<Message> parts, SendFlags flags) { record("send." + firstPart(parts)); return true; }
        @Override public boolean request(List<Message> parts, ZLinkBackendRequestCallback callback, SendFlags flags, Duration timeout) {
            record("request." + firstPart(parts));
            Message reply = jsonStringMessage("reply");
            try {
                callback.handle(new ZLinkBackendReceived(
                    Optional.empty(),
                    Optional.empty(),
                    Optional.empty(),
                    List.of(Message.from(reply))));
            } finally {
                reply.close();
            }
            return true;
        }
        @Override public ZLinkBackendReceived recv(ZLinkBackendRecvMode mode) { return null; }
    }

    private static final class FakeRouterSocket extends FakeConnectableSocket implements ZLinkBackendRouterSocket {
        private final Deque<ZLinkBackendReceived> received = new ArrayDeque<>();
        private final Semaphore readable = new Semaphore(0);
        private long maxMessageSize;
        private int peerWeight = 100;

        FakeRouterSocket(List<String> calls, String name) {
            super(calls, name);
        }

        void enqueueReceived(
            RoutingId sourceRid,
            Optional<Long> requestSeq,
            List<Message> parts) {
            received.add(new ZLinkBackendReceived(
                Optional.of(sourceRid),
                Optional.empty(),
                requestSeq,
                parts,
                replyParts -> record("reply." + sourceRid + "." + (replyParts.isEmpty()
                    ? ""
                    : firstPart(replyParts)))));
            readable.release();
        }

        @Override public void setChannelName(String channelName) { record("setChannelName." + channelName); }
        @Override public void setRoutingId(RoutingId routingId) { record("setRoutingId"); }
        @Override public void setConnectRoutingId(RoutingId routingId) { record("setConnectRoutingId"); }
        @Override public void setProbe(boolean enabled) { record("setProbe." + enabled); }
        @Override public long maxMessageSize() { return maxMessageSize; }
        @Override public void setMaxMessageSize(long value) { maxMessageSize = value; record("setMaxMessageSize." + value); }
        @Override public int peerWeight() { return peerWeight; }
        @Override public void setPeerWeight(int weight) { peerWeight = weight; record("setPeerWeight." + weight); }
        @Override public boolean waitForReadable(Duration timeout) {
            try {
                return readable.tryAcquire(timeout.toNanos(), TimeUnit.NANOSECONDS);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                return false;
            }
        }
        @Override public ZLinkBackendReceived recv(ZLinkBackendRecvMode mode) { return received.pollFirst(); }
        @Override public boolean send(RoutingId routingId, List<Message> parts, SendFlags flags) { record("send." + routingId + "." + firstPart(parts)); return true; }
        @Override public boolean request(RoutingId routingId, List<Message> parts, ZLinkBackendRequestCallback callback, SendFlags flags, Duration timeout) {
            record("request." + routingId + "." + firstPart(parts));
            if (isRoutedActorJoinRequest(parts)) {
                ZLinkActorSpotRoutePackets.TransferRequest request =
                    ZLinkActorSpotRoutePackets.decodeTransferRequest(parts.get(1));
                List<Message> routeReply;
                Message joinedPayload = jsonStringMessage("joined");
                Message rejectedPayload = Message.from(new byte[0]);
                Message joinReply = null;
                try {
                    boolean accepted = !request.actorId().contains("reject");
                    joinReply = request.admission()
                        ? ZLinkActorSpotRoutePackets.encodeAdmissionReply(
                            accepted,
                            accepted ? joinedPayload : rejectedPayload)
                        : ZLinkActorSpotRoutePackets.encodeJoinReply(
                            true,
                            new ZLinkBackendActorRef(
                                routingId,
                                request.actorId(),
                                request.actorGeneration()),
                            rejectedPayload);
                    routeReply = List.of(Message.from(joinReply));
                } finally {
                    if (joinReply != null) {
                        joinReply.close();
                    }
                    joinedPayload.close();
                    rejectedPayload.close();
                }
                callback.handle(new ZLinkBackendReceived(
                    Optional.empty(),
                    Optional.empty(),
                    Optional.empty(),
                    routeReply));
                return true;
            }
            if (isRoutedBoundSessionSendRequest(parts)) {
                callback.handle(new ZLinkBackendReceived(
                    Optional.empty(),
                    Optional.empty(),
                    Optional.empty(),
                    List.of()));
                return true;
            }
            Message reply = jsonStringMessage("reply");
            try {
                callback.handle(new ZLinkBackendReceived(
                    Optional.empty(),
                    Optional.empty(),
                    Optional.empty(),
                    List.of(Message.from(reply))));
            } finally {
                reply.close();
            }
            return true;
        }
        @Override public void reply(RoutingId routingId, long requestSeq, List<Message> parts) { record("reply"); }
    }

    private static final class FakePublisherSocket extends FakeSocket implements ZLinkBackendPublisherSocket {
        FakePublisherSocket(List<String> calls, String name) {
            super(calls, name);
        }

        @Override public void setChannelName(String channelName) { record("setChannelName." + channelName); }
        @Override public void setRoutingId(RoutingId routingId) { record("setRoutingId"); }
        @Override public boolean publish(String topic, List<Message> parts, SendFlags flags) { record("publish." + topic + "." + firstPart(parts)); return true; }
    }

    private static final class FakeSubscriberSocket extends FakeConnectableSocket implements ZLinkBackendSubscriberSocket {
        private final Semaphore readable = new Semaphore(0);

        FakeSubscriberSocket(List<String> calls, String name) {
            super(calls, name);
        }

        @Override public void setChannelName(String channelName) { record("setChannelName." + channelName); }
        @Override public void setSubscription(String topic) { record("setSubscription." + topic); }
        @Override public boolean waitForReadable(Duration timeout) {
            try {
                return readable.tryAcquire(timeout.toNanos(), TimeUnit.NANOSECONDS);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                return false;
            }
        }
        @Override public ZLinkBackendTopicMessage subscribe(ZLinkBackendRecvMode mode) { return null; }
    }

    private static final class FakeSpotNode extends FakeBackendObject implements ZLinkInternalSpotNode {
        private int nextSpotId = 1;
        private final List<FakeSpot> spots;
        private final FakeZLinkBackendAdapterFactory owner;
        private RoutingId routingId = RoutingId.from("spot-node");
        private FakeSpot entrySpot;

        FakeSpotNode(
            List<String> calls,
            List<FakeSpot> spots,
            FakeZLinkBackendAdapterFactory owner) {
            super(calls, "spotNode");
            this.spots = spots;
            this.owner = owner;
        }

        @Override public RoutingId routingId() { return routingId; }
        @Override public void setRoutingId(RoutingId routingId) { this.routingId = routingId; record("setRoutingId"); }
        @Override public void setPublisherRoutingId(RoutingId routingId) { record("setPublisherRoutingId"); }
        @Override public void setSubscriberRoutingId(RoutingId routingId) { record("setSubscriberRoutingId"); }
        @Override public void setRouterBind(String endpoint) { record("setRouterBind." + endpoint); }
        @Override public void setPubBind(String endpoint) { record("setPubBind." + endpoint); }
        @Override public void connectPeer(String endpoint) { record("connectPeer." + endpoint); }
        @Override public void connectPeer(RoutingId peerRid, String endpoint) { record("connectPeer." + peerRid + "." + endpoint); }
        @Override public void disconnectPeer(String endpoint) { record("disconnectPeer." + endpoint); }
        @Override public void disconnectPeer(RoutingId peerRid) { record("disconnectPeer." + peerRid); }
        @Override public void publish(
            String channelName,
            String topic,
            List<Message> parts,
            SendFlags flags) {
            record("publish." + channelName + "." + topic + "." + firstPart(parts));
        }
        @Override public void publish(
            String channelName,
            String topic,
            byte[] metadata,
            List<Message> parts,
            SendFlags flags) {
            owner.captureApplicationMetadata(metadata);
            record("publish." + channelName + "." + topic + "." + firstPart(parts));
        }
        @Override public ZLinkBackendSpotRouteBridge createRouteBridge() {
            record("createRouteBridge");
            return new FakeSpotRouteBridge(calls());
        }
        @Override public ZLinkBackendSpot createSpot() {
            record("createSpot");
            FakeSpot spot = new FakeSpot(calls(), "spot." + nextSpotId++, owner);
            spots.add(spot);
            return spot;
        }
        @Override public ZLinkBackendSpot entrySpot() {
            if (entrySpot == null) {
                record("entrySpot");
                entrySpot = new FakeSpot(calls(), "entrySpot", owner);
                spots.add(entrySpot);
            }
            return entrySpot;
        }
        @Override public ZLinkBackendActorRef createActor(String actorId, Message createRequest) {
            if (createRequest != null) {
                createRequest.close();
            }
            record("createActor." + actorId);
            ZLinkBackendActorRef actor = new ZLinkBackendActorRef(routingId(), actorId, 0);
            owner.actors.put(actorId, actor);
            return actor;
        }
        @Override public ZLinkBackendActorRef actorLookup(String actorId) {
            record("actorLookup." + actorId);
            ZLinkBackendActorRef actor = owner.actors.get(actorId);
            if (actor == null) {
                throw new ZlinkConfigException(ConfigResult.NOT_FOUND);
            }
            return actor;
        }
        @Override public CompletionStage<ZLinkBackendActorJoinResult> joinActor(
            ZLinkBackendActorRef actor,
            RoutingId targetNodeRid,
            String targetSpotId,
            List<Message> parts,
            Duration timeout) {
            record("joinActor." + actor.actorId() + "." + targetNodeRid + "." + targetSpotId);
            FakeSpot localSpot = owner.spots.stream()
                .filter(spot -> spot.spotId().equals(targetSpotId))
                .findFirst()
                .orElse(null);
            if (localSpot != null && routingId.equals(targetNodeRid)) {
                ZLinkBackendActorRef targetActor = new ZLinkBackendActorRef(
                    routingId,
                    actor.actorId(),
                    actor.generation());
                CompletableFuture<FakeActorJoinReply> admitted = new CompletableFuture<>();
                localSpot.enqueueActorJoin(actor, targetActor, parts, admitted);
                localSpot.dispatchActorJoinReadable();
                return admitted.thenApply(reply -> new ZLinkBackendActorJoinResult(
                    ZLinkBackendRequestResult.OK,
                    reply.resultCode(),
                    targetActor,
                    targetSpotId,
                    1,
                    0,
                    reply.parts()));
            }
            RoutingId joinedNodeRid = targetSpotId.toString().contains("native-remote")
                ? RoutingId.from("native-remote-node")
                : targetNodeRid;
            Message reply = owner.nextActorJoinReply == null
                ? jsonStringMessage("joined")
                : Message.from(owner.nextActorJoinReply);
            if (owner.nextActorJoinReply != null) {
                owner.nextActorJoinReply.close();
                owner.nextActorJoinReply = null;
            }
            return CompletableFuture.completedFuture(new ZLinkBackendActorJoinResult(
                ZLinkBackendRequestResult.OK,
                0,
                new ZLinkBackendActorRef(joinedNodeRid, actor.actorId(), actor.generation()),
                targetSpotId,
                1,
                0,
                List.of(reply)));
        }
        @Override public CompletionStage<ZLinkBackendActorJoinEntrySpotResult> joinActorEntrySpot(ZLinkBackendActorRef actor, RoutingId targetNodeRid, Message request, Duration timeout) {
            record("joinActorEntrySpot." + actor.actorId() + "." + targetNodeRid);
            return CompletableFuture.completedFuture(new ZLinkBackendActorJoinEntrySpotResult(
                ZLinkBackendRequestResult.OK,
                0,
                new ZLinkBackendActorRef(targetNodeRid, actor.actorId(), actor.generation()),
                targetNodeRid,
                targetNodeRid.toString(),
                1,
                0,
                List.of(Message.from("entry-joined".getBytes(StandardCharsets.UTF_8)))));
        }
        @Override public CompletionStage<List<Message>> leaveActor(ZLinkBackendActorRef actor, String currentSpotId, Duration timeout) {
            record("leaveActor." + actor.actorId() + "." + currentSpotId);
            return CompletableFuture.completedFuture(List.of());
        }
        @Override public CompletionStage<Void> destroyActor(ZLinkBackendActorRef actor, Duration timeout) {
            record("destroyActor." + actor.actorId());
            owner.actors.remove(actor.actorId());
            return CompletableFuture.completedFuture(null);
        }
        @Override public PrepareActorTransferResult prepareActorTransfer(
            ActorTransferPrepare prepare,
            Duration timeout) {
            record("prepareActorTransfer." + prepare.actor().actorId());
            ActorTransferToken token =
                ActorTransferTokenFixture.create(new byte[64]);
            return new PrepareActorTransferResult(
                token,
                new ActorTransferPrepareResult(
                    prepare.role(),
                    prepare.transferId(),
                    prepare.actor(),
                    prepare.finalSequence(),
                    prepare.reserveMessageCount(),
                    prepare.reserveByteCount()));
        }
        @Override public void commitActorTransfer(
            ActorTransferToken token,
            long newMembershipEpoch) {
            record("commitActorTransfer." + newMembershipEpoch);
        }
        @Override public void activateActorTransfer(ActorTransferToken token) {
            record("activateActorTransfer");
        }
        @Override public void abortActorTransfer(ActorTransferToken token) {
            record("abortActorTransfer");
        }
        @Override public void registerTransferredActor(
            ZLinkBackendActorRef actor,
            String spotId,
            long membershipEpoch) {
            record("registerTransferredActor." + actor.actorId());
            owner.actors.put(actor.actorId(), actor);
        }
        @Override public boolean sendActorBoundSession(ZLinkBackendActorRef actor, List<Message> parts, SendFlags flags) {
            record("sendActorBoundSession." + actor.actorId() + "." + firstPart(parts));
            return true;
        }
        @Override public void replyActorNoBind(
            ZLinkBackendActorRef actor,
            RoutingId sourceNodeRid,
            RoutingId sourceSessionRid,
            long requestId,
            int flags,
            List<Message> parts) {
            record("replyActorNoBind."
                + actor.actorId()
                + "."
                + sourceNodeRid
                + "."
                + sourceSessionRid
                + "."
                + requestId
                + "."
                + flags
                + "."
                + firstPart(parts));
        }
        @Override public boolean sendToActor(ZLinkBackendActorRef actor, List<Message> parts, SendFlags flags) {
            record("sendToActor." + actor.actorId() + "." + firstPart(parts));
            return true;
        }
        @Override public CompletionStage<List<Message>> requestToActor(
            ZLinkBackendActorRef actor,
            List<Message> parts,
            SendFlags flags,
            Duration timeout) {
            record("requestToActor." + actor.actorId() + "." + firstPart(parts));
            return CompletableFuture.completedFuture(List.of(jsonStringMessage("actor-reply")));
        }
        @Override public boolean forwardActorBoundSession(
            ZLinkBackendActorRef actor,
            RoutingId sourceNodeRid,
            RoutingId sourceSessionRid,
            List<Message> parts,
            SendFlags flags) {
            record("forwardActorBoundSession."
                + actor.actorId()
                + "."
                + sourceNodeRid
                + "."
                + sourceSessionRid
                + "."
                + firstPart(parts));
            return true;
        }
        @Override public void bindRemoteActorBoundSession(
            ZLinkBackendActorRef actor,
            RoutingId sourceNodeRid,
            RoutingId sourceSessionRid) {
            record("bindRemoteActorBoundSession."
                + actor.actorId()
                + "."
                + sourceNodeRid
                + "."
                + sourceSessionRid);
        }
        @Override public void closeActorBoundSession(ZLinkBackendActorRef actor, Duration timeout) {
            record("closeActorBoundSession." + actor.actorId());
        }
    }

    private static final class FakeSpotRouteBridge extends FakeBackendObject implements ZLinkBackendSpotRouteBridge {
        FakeSpotRouteBridge(List<String> calls) {
            super(calls, "spotRouteBridge");
        }

        @Override public void attachRouterChannel(String channelName, ZLinkBackendRouterSocket router) {
            record("bridge.attachRouterChannel." + channelName);
        }

        @Override public boolean send(String channelName, RoutingId targetNodeRid, String targetSpotId, List<Message> parts, SendFlags flags) {
            record("bridge.send." + channelName + "." + targetNodeRid + "." + targetSpotId + "." + firstPart(parts));
            return true;
        }

        @Override public boolean request(String channelName, RoutingId targetNodeRid, String targetSpotId, List<Message> parts, ZLinkBackendRequestCallback callback, SendFlags flags, Duration timeout) {
            record("bridge.request." + channelName + "." + targetNodeRid + "." + targetSpotId + "." + firstPart(parts));
            if (isRoutedActorJoinRequest(parts)) {
                ZLinkActorSpotRoutePackets.TransferRequest request =
                    ZLinkActorSpotRoutePackets.decodeTransferRequest(parts.get(1));
                Message joinedPayload = jsonStringMessage("joined");
                Message rejectedPayload = Message.from(new byte[0]);
                Message joinReply = null;
                try {
                    boolean accepted = !request.actorId().contains("reject");
                    joinReply = request.admission()
                        ? ZLinkActorSpotRoutePackets.encodeAdmissionReply(
                            accepted,
                            accepted ? joinedPayload : rejectedPayload)
                        : ZLinkActorSpotRoutePackets.encodeJoinReply(
                            true,
                            new ZLinkBackendActorRef(
                                targetNodeRid,
                                request.actorId(),
                                request.actorGeneration()),
                            rejectedPayload);
                    callback.handle(new ZLinkBackendReceived(
                        Optional.empty(),
                        Optional.empty(),
                        Optional.empty(),
                        List.of(Message.from(joinReply))));
                    return true;
                } finally {
                    if (joinReply != null) {
                        joinReply.close();
                    }
                    joinedPayload.close();
                    rejectedPayload.close();
                }
            }
            if (isRoutedBoundSessionSendRequest(parts)) {
                callback.handle(new ZLinkBackendReceived(
                    Optional.empty(),
                    Optional.empty(),
                    Optional.empty(),
                    List.of()));
                return true;
            }
            Message reply = jsonStringMessage("reply");
            try {
                callback.handle(new ZLinkBackendReceived(
                    Optional.empty(),
                    Optional.empty(),
                    Optional.empty(),
                    List.of(Message.from(reply))));
            } finally {
                reply.close();
            }
            return true;
        }

        @Override public boolean handleRouterReceived(String channelName, RoutingId sourceNodeRid, long requestSeq, List<Message> parts) {
            record("bridge.handleRouterReceived." + channelName + "." + firstPart(parts));
            return true;
        }

        @Override public int drain() {
            record("bridge.drain");
            return 0;
        }

        @Override public void close() {
            record("bridge.close");
        }
    }

    private static final class FakeSpot extends FakeBackendObject implements ZLinkBackendSpot {
        private final FakeZLinkBackendAdapterFactory owner;
        private String routingId;
        private final Deque<ZLinkBackendActorJoinRequest> actorJoins = new ArrayDeque<>();
        private final Map<String, CompletableFuture<FakeActorJoinReply>> actorJoinReplies =
            new ConcurrentHashMap<>();
        private final Deque<ZLinkBackendActorLifecycleEvent> actorLifecycles = new ArrayDeque<>();
        private final Deque<ZLinkBackendReceived> routes = new ArrayDeque<>();
        private final Deque<ZLinkBackendTopicMessage> subscriptions = new ArrayDeque<>();
        private final List<String> replies = new ArrayList<>();
        private ZLinkBackendSpotDispatchHandler dispatchHandler;

        FakeSpot(
            List<String> calls,
            String name,
            FakeZLinkBackendAdapterFactory owner) {
            super(calls, name);
            this.owner = owner;
            this.routingId = name;
        }

        void enqueueActorJoin(String actorId, String packetName, String payload) {
            List<Message> parts = packetName == null
                ? List.of(Message.from(payload.getBytes(StandardCharsets.UTF_8)))
                : List.of(jsonStringMessage(payload));
            ZLinkBackendActorRef targetActor =
                new ZLinkBackendActorRef(RoutingId.from("spot-node"), actorId, 1);
            owner.actors.put(actorId, targetActor);
            actorJoins.add(new ZLinkBackendActorJoinRequest(
                new ZLinkBackendActorRef(RoutingId.from("source-node"), actorId, 1),
                targetActor,
                parts,
                null));
        }

        void enqueueActorJoin(
            ZLinkBackendActorRef sourceActor,
            ZLinkBackendActorRef targetActor,
            List<Message> parts,
            CompletableFuture<FakeActorJoinReply> reply) {
            owner.actors.put(targetActor.actorId(), targetActor);
            actorJoins.add(new ZLinkBackendActorJoinRequest(
                sourceActor,
                targetActor,
                parts.stream().map(Message::from).toList(),
                null));
            actorJoinReplies.put(targetActor.actorId(), reply);
        }

        void dispatchActorJoinReadable() {
            if (dispatchHandler == null) {
                throw new IllegalStateException("fake spot dispatch handler is not registered");
            }
            dispatchHandler.handle(new ZLinkBackendSpotDispatchInfo(
                ZLinkBackendSpotDispatchEvent.ACTOR_JOIN_READABLE,
                List.of()));
        }

        void enqueueActorLifecycleLeft(String actorId) {
            ZLinkBackendActorRef actor =
                new ZLinkBackendActorRef(RoutingId.from("spot-node"), actorId, 1);
            actorLifecycles.add(new ZLinkBackendActorLifecycleEvent(
                ZLinkBackendActorLifecycleEventKind.LEFT,
                new ZLinkBackendActorLifecycleInfo(
                    actor,
                    actor,
                    Optional.of(spotId()),
                    Optional.empty(),
                    1,
                    0)));
        }

        void enqueueActorLifecycleJoined(String actorId, String spotId) {
            ZLinkBackendActorRef actor =
                new ZLinkBackendActorRef(RoutingId.from("spot-node"), actorId, 1);
            actorLifecycles.add(new ZLinkBackendActorLifecycleEvent(
                ZLinkBackendActorLifecycleEventKind.JOINED,
                new ZLinkBackendActorLifecycleInfo(
                    actor,
                    actor,
                    Optional.empty(),
                    Optional.of(spotId),
                    0,
                    1)));
        }

        void dispatchActorLifecycleReadable() {
            if (dispatchHandler == null) {
                throw new IllegalStateException("fake spot dispatch handler is not registered");
            }
            dispatchHandler.handle(new ZLinkBackendSpotDispatchInfo(
                ZLinkBackendSpotDispatchEvent.ACTOR_LIFECYCLE_READABLE,
                List.of()));
        }

        void enqueueRoute(String packetName, String payload, Optional<Long> requestSeq) {
            enqueueRoute(packetName, payload, requestSeq, new byte[0]);
        }

        void enqueueRoute(
            String packetName,
            String payload,
            Optional<Long> requestSeq,
            byte[] metadata) {
            routes.add(new ZLinkBackendReceived(
                ZLinkBackendRequestResult.OK,
                Optional.of(RoutingId.from("source")),
                Optional.of(name()),
                requestSeq,
                metadata,
                List.of(Message.from(packetName), Message.from(payload)),
                replyParts -> {
                    String reply = replyParts.isEmpty()
                        ? ""
                        : replyParts.get(0).toUtf8String();
                    replies.add(reply);
                    owner.spotReplyObserver.accept(reply);
                },
                () -> { }));
        }

        void dispatchRouteReadable() {
            if (dispatchHandler == null) {
                throw new IllegalStateException("fake spot dispatch handler is not registered");
            }
            dispatchHandler.handle(new ZLinkBackendSpotDispatchInfo(
                ZLinkBackendSpotDispatchEvent.ROUTED_READABLE,
                List.of()));
        }

        void enqueueSubscription(String topic, String packetName, String payload) {
            enqueueSubscription(topic, packetName, payload, new byte[0]);
        }

        void enqueueSubscription(
            String topic,
            String packetName,
            String payload,
            byte[] metadata) {
            subscriptions.add(new ZLinkBackendTopicMessage(
                Optional.of(RoutingId.from("publisher")),
                null,
                topic,
                metadata,
                List.of(Message.from(packetName), Message.from(payload))));
        }

        void dispatchSubscribeReadable() {
            if (dispatchHandler == null) {
                throw new IllegalStateException("fake spot dispatch handler is not registered");
            }
            dispatchHandler.handle(new ZLinkBackendSpotDispatchInfo(
                ZLinkBackendSpotDispatchEvent.SUBSCRIBE_READABLE,
                List.of()));
        }

        List<String> replies() {
            return List.copyOf(replies);
        }

        void dispatchActorMessage(
            String actorId,
            String packetName,
            String payload,
            Optional<Long> requestSeq) {
            dispatchActorMessage(
                actorId,
                packetName.getBytes(StandardCharsets.UTF_8),
                payload,
                requestSeq);
        }

        void dispatchActorMessage(
            String actorId,
            byte[] header,
            String payload,
            Optional<Long> requestSeq) {
            if (dispatchHandler == null) {
                throw new IllegalStateException("fake spot dispatch handler is not registered");
            }
            ZLinkBackendActorRef actor =
                new ZLinkBackendActorRef(RoutingId.from("spot-node"), actorId, 1);
            dispatchHandler.handle(new ZLinkBackendSpotDispatchInfo(
                ZLinkBackendSpotDispatchEvent.ACTOR_READABLE,
                List.of(
                    new ZLinkBackendActorReceived(
                        actor,
                        RoutingId.from("source-node"),
                        RoutingId.from("source-session"),
                        requestSeq,
                        0,
                        0,
                        Message.from(header),
                        true),
                    new ZLinkBackendActorReceived(
                        actor,
                        RoutingId.from("source-node"),
                        RoutingId.from("source-session"),
                        requestSeq,
                        0,
                        0,
                        Message.from(payload.getBytes(StandardCharsets.UTF_8)),
                        false))));
        }

        @Override public String spotId() { return routingId; }
        @Override public void setRoutingId(String spotId) {
            this.routingId = spotId;
            record("setRoutingId");
        }
        @Override public void setSubscription(String topic) { record("setSubscription." + topic); }
        @Override public ZLinkBackendTopicMessage subscribe(ZLinkBackendRecvMode mode) { return subscriptions.pollFirst(); }
        @Override public ZLinkBackendReceived recvRoute(ZLinkBackendRecvMode mode) { return routes.pollFirst(); }
        @Override public boolean publish(String channelName, String topic, List<Message> parts, SendFlags flags) { record("publish." + channelName + "." + topic + "." + firstPart(parts)); return true; }
        @Override public boolean publish(
            String channelName,
            String topic,
            byte[] metadata,
            List<Message> parts,
            SendFlags flags) {
            owner.captureApplicationMetadata(metadata);
            return publish(channelName, topic, parts, flags);
        }
        @Override public boolean sendToSpot(RoutingId targetNodeRid, String spotId, long spotGeneration, List<Message> parts, SendFlags flags) { record("sendToSpot." + targetNodeRid + "." + spotId + "." + firstPart(parts)); return true; }
        @Override public boolean sendToSpot(
            RoutingId targetNodeRid,
            String spotId,
            long spotGeneration,
            byte[] metadata,
            List<Message> parts,
            SendFlags flags) {
            owner.captureApplicationMetadata(metadata);
            return sendToSpot(
                targetNodeRid, spotId, spotGeneration, parts, flags);
        }
        @Override public boolean requestToSpot(RoutingId targetNodeRid, String spotId, long spotGeneration, List<Message> parts, ZLinkBackendRequestCallback callback, SendFlags flags, Duration timeout) {
            record("requestToSpot." + targetNodeRid + "." + spotId + "." + firstPart(parts));
            List<Message> replyParts = owner.nextSpotRequestReplyParts == null
                ? List.of(jsonStringMessage("reply"))
                : owner.nextSpotRequestReplyParts.stream().map(Message::from).toList();
            callback.handle(new ZLinkBackendReceived(
                Optional.empty(),
                Optional.empty(),
                Optional.empty(),
                replyParts));
            return true;
        }
        @Override public boolean requestToSpot(
            RoutingId targetNodeRid,
            String spotId,
            long spotGeneration,
            byte[] metadata,
            List<Message> parts,
            ZLinkBackendRequestCallback callback,
            SendFlags flags,
            Duration timeout) {
            owner.captureApplicationMetadata(metadata);
            return requestToSpot(
                targetNodeRid,
                spotId,
                spotGeneration,
                parts,
                callback,
                flags,
                timeout);
        }
        @Override public void onDispatchEvent(ZLinkBackendSpotDispatchHandler handler) {
            dispatchHandler = handler;
            record("onDispatchEvent");
        }
        @Override public ZLinkBackendActorJoinRequest recvActorJoin(ZLinkBackendRecvMode mode) {
            record("recvActorJoin." + mode);
            return actorJoins.pollFirst();
        }
        @Override public void replyActorJoin(
            ZLinkBackendActorJoinRequest request,
            int joinResultCode,
            List<Message> parts) {
            record("replyActorJoin." + request.targetActor().actorId() + "." + joinResultCode);
            record("replyActorJoinPayload." + request.targetActor().actorId() + "." + joinResultCode
                + "." + firstPart(parts));
            CompletableFuture<FakeActorJoinReply> pending =
                actorJoinReplies.remove(request.targetActor().actorId());
            if (pending != null) {
                List<Message> replyParts;
                if (owner.nextActorJoinReply != null) {
                    replyParts = List.of(Message.from(owner.nextActorJoinReply));
                    owner.nextActorJoinReply.close();
                    owner.nextActorJoinReply = null;
                } else if (parts.isEmpty()) {
                    replyParts = List.of(jsonStringMessage("joined"));
                } else {
                    replyParts = parts.stream().map(Message::from).toList();
                }
                pending.complete(new FakeActorJoinReply(joinResultCode, replyParts));
            }
        }
        @Override public ZLinkBackendActorLifecycleEvent recvActorLifecycle(
            ZLinkBackendRecvMode mode) {
            record("recvActorLifecycle." + mode);
            return actorLifecycles.pollFirst();
        }
    }

    private static final class FakeStreamSocket extends FakeSocket implements ZLinkBackendStreamSocket {
        private final Queue<ZLinkBackendStreamReceived> received =
            new ConcurrentLinkedQueue<>();
        private final Semaphore readable = new Semaphore(0);
        private ZLinkBackendStreamErrorHandler errorHandler;

        FakeStreamSocket(List<String> calls) {
            super(calls, "stream");
        }

        @Override public void setTlsServer(
            String certificatePath,
            String keyPath,
            boolean requireClientCertificate) {
            record("setTlsServer." + certificatePath + "." + keyPath + "."
                + requireClientCertificate);
        }
        @Override public void setMaxMessageSize(long value) {
            record("setMaxMessageSize." + value);
        }
        @Override public void enableNotifications() { record("enableNotifications"); }
        @Override public boolean waitForReadable(Duration timeout) {
            try {
                return readable.tryAcquire(timeout.toNanos(), TimeUnit.NANOSECONDS);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                return false;
            }
        }
        @Override public ZLinkBackendStreamReceived recv() {
            record("recv");
            return received.poll();
        }
        @Override public void onTransportError(ZLinkBackendStreamErrorHandler handler) { errorHandler = handler; record("onTransportError"); }
        @Override public void startSessionService() { record("startSessionService"); }
        @Override public boolean send(RoutingId routingId, List<Message> parts, SendFlags flags) {
            record("send." + routingId + "." + firstPart(parts));
            return true;
        }
        @Override public boolean send(RoutingId routingId, String packetName, List<Message> parts, SendFlags flags) {
            record("send." + routingId + "." + packetName + "." + firstPart(parts));
            return true;
        }
        @Override public boolean send(RoutingId routingId, ZLinkStreamHeader header, List<Message> parts, SendFlags flags) {
            record("send." + routingId + "." + header.packetName() + "." + header.flags() + "." + header.metadata() + "." + firstPart(parts));
            return true;
        }
        @Override public boolean reply(RoutingId routingId, long requestSeq, String packetName, List<Message> parts, SendFlags flags) {
            record("reply." + routingId + "." + requestSeq + "." + packetName + "." + firstPart(parts));
            return true;
        }
        @Override public boolean reply(RoutingId routingId, ZLinkStreamHeader header, List<Message> parts, SendFlags flags) {
            record("reply." + routingId + "." + header.requestSequence().orElse(0L) + "." + header.packetName() + "." + header.flags() + "." + header.metadata() + "." + firstPart(parts));
            return true;
        }
        @Override public ZLinkBackendActorBindOperation bindActor(RoutingId sessionRid, ZLinkBackendActorRef actor) { record("bindActor." + actor.actorId()); return timeout -> CompletableFuture.completedFuture(null); }
        @Override public ZLinkBackendActorUnbindOperation unbindActor(RoutingId sessionRid, String actorId) { record("unbindActor." + actorId); return timeout -> CompletableFuture.completedFuture(null); }
        @Override public boolean sendBoundActor(RoutingId sessionRid, String actorId, List<Message> parts, SendFlags flags) { record("sendBoundActor." + actorId); return true; }
        @Override public boolean relayBoundActor(RoutingId sessionRid, String actorId, ZLinkStreamHeader header, List<Message> parts, SendFlags flags) { record("relayBoundActor." + actorId + "." + header.codec() + "." + header.packetName()); return true; }

        void dispatchPacket(RoutingId routingId, Message header, Message payload) {
            byte[] headerBytes = header.toByteArray();
            byte[] payloadBytes = payload.toByteArray();
            byte[] frame = ByteBuffer.allocate(6 + headerBytes.length + payloadBytes.length)
                .putShort((short) headerBytes.length)
                .putInt(payloadBytes.length)
                .put(headerBytes)
                .put(payloadBytes)
                .array();
            Message raw = Message.from(frame);
            received.add(new ZLinkBackendStreamReceived(
                Optional.of(routingId),
                List.of(raw),
                raw::close));
            readable.release();
        }

        @Override public void close() {
            readable.release();
            ZLinkBackendStreamReceived next;
            while ((next = received.poll()) != null) {
                next.close();
            }
            super.close();
        }

        void dispatchTransportError(RoutingId routingId, int nativeCode, String message) {
            if (errorHandler == null) {
                throw new IllegalStateException("stream error handler is not registered");
            }
            errorHandler.handle(routingId, nativeCode, message);
        }
    }

    private static final class FakeSocketMonitor extends FakeBackendObject implements ZLinkBackendSocketMonitor {
        FakeSocketMonitor(List<String> calls) {
            super(calls, "socketMonitor");
        }

        @Override public void onEvent(ZLinkBackendSocketMonitorHandler handler) { record("onEvent"); }
        @Override public ZLinkBackendSocketMonitorEvent recv() { return null; }
    }

    private static String firstPart(List<Message> parts) {
        return parts.isEmpty() ? "" : parts.get(0).toUtf8String();
    }

    private static boolean isRoutedActorJoinRequest(List<Message> parts) {
        return parts.size() >= 2
            && ZLinkActorSpotRoutePackets.JOIN_SPOT_PACKET_NAME.equals(firstPart(parts));
    }

    private static boolean isRoutedBoundSessionSendRequest(List<Message> parts) {
        return parts.size() >= 1
            && ZLinkActorSpotRoutePackets.BOUND_SESSION_SEND_PACKET_NAME.equals(firstPart(parts));
    }

    private static List<Message> copyMessages(List<Message> parts) {
        return parts.stream()
            .map(Message::from)
            .toList();
    }
}
