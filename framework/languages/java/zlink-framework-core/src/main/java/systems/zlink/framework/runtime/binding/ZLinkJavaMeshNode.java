package systems.zlink.framework.runtime.binding;

import java.util.ArrayList;
import java.time.Duration;
import java.util.List;
import java.util.function.Consumer;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executors;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNode;
import systems.zlink.framework.runtime.internal.binding.spot.ActorControlRecord;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry;
import systems.zlink.framework.runtime.internal.binding.spot.PeerChannels;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeMonitor;
import systems.zlink.framework.runtime.internal.binding.spot.OperationId;
import systems.zlink.framework.runtime.internal.binding.spot.RecordKind;
import systems.zlink.framework.runtime.internal.binding.spot.ReadyDomain;
import systems.zlink.framework.runtime.internal.binding.spot.ReplyToken;
import systems.zlink.framework.runtime.internal.binding.spot.Dispatch;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshApplicationReceiver;

final class ZLinkJavaMeshNode implements ZLinkInternalMeshNode {
    private final MeshNode node;
    private final String meshName;
    private final List<Long> connectionIntentIds = new ArrayList<>();
    private final java.util.concurrent.ConcurrentMap<Long, RoutingId> expectedConnectionRids =
        new java.util.concurrent.ConcurrentHashMap<>();
    private final java.util.Set<Long> untypedConnectionIntents =
        java.util.concurrent.ConcurrentHashMap.newKeySet();
    private final java.util.concurrent.ConcurrentMap<String, Integer> channelWeights =
        new java.util.concurrent.ConcurrentHashMap<>();
    private final ZLinkJavaMeshOperationTracker operations =
        new ZLinkJavaMeshOperationTracker();
    private final java.util.concurrent.ConcurrentMap<Long, ReplyToken> actorReplies =
        new java.util.concurrent.ConcurrentHashMap<>();
    private ZLinkJavaMeshDispatchPump dispatchPump;
    private Consumer<ZLinkMeshDispatchRecord> applicationReceiver =
        ZLinkMeshDispatchRecord::close;
    private java.util.function.Function<ZLinkMeshDispatchRecord, CompletionStage<Void>>
        serviceDispatchReceiver = record -> {
            record.close();
            return CompletableFuture.completedFuture(null);
        };
    private ZLinkJavaMeshSpotNode spotNode;
    private String primaryChannelName;
    private ZLinkMeshApplicationReceiver localApplicationReceiver;
    private Duration routerSendTimeout = Duration.ofSeconds(1);
    private volatile int placementWeight = 100;
    private int routerPendingAdmissionCapacity = 4096;

    ZLinkJavaMeshNode(MeshNode node, String meshName) {
        this.node = node;
        this.meshName = meshName;
    }

    @Override
    public String name() {
        return meshName;
    }

    @Override
    public void setBind(String endpoint) {
        node.setBind(endpoint);
    }

    @Override
    public void addChannel(String channelName) {
        node.addChannel(channelName);
        channelWeights.put(channelName, 100);
        if (primaryChannelName == null) {
            primaryChannelName = channelName;
        }
        if (spotNode != null) {
            spotNode.channelName(channelName);
        }
    }

    @Override
    public void setChannelWeight(String channelName, int weight) {
        node.setChannelWeight(channelName, weight);
        channelWeights.put(channelName, weight);
    }

    @Override
    public int placementWeight() {
        return placementWeight;
    }

    @Override
    public void setPlacementWeight(int weight) {
        if (weight < 0 || weight > 10_000) {
            throw new IllegalArgumentException(
                "placement weight must be in 0..10000");
        }
        placementWeight = weight;
    }

    @Override
    public long maxMessageSize() {
        return Math.max(0, node.maxMessageSize());
    }

    @Override
    public void setMaxMessageSize(long value) {
        node.setMaxMessageSize(value == 0 ? -1 : value);
    }

    @Override
    public void setRouterHighWaterMark(long value) {
        node.setRouterHighWaterMark(value);
    }

    @Override
    public synchronized void setRouterSendTimeout(Duration value) {
        routerSendTimeout = java.util.Objects.requireNonNull(value, "value");
        if (spotNode != null) {
            spotNode.setAdmissionTimeout(value);
        }
    }

    @Override
    public synchronized void setRouterPendingAdmissionCapacity(int value) {
        if (value <= 0) {
            throw new IllegalArgumentException(
                "pending admission capacity must be positive");
        }
        routerPendingAdmissionCapacity = value;
        if (spotNode != null) {
            spotNode.setAdmissionPendingCapacity(value);
        }
    }

    @Override
    public void setMailboxMessageBudget(long value) {
        node.setMailboxMessageBudget(value);
    }

    @Override
    public void setRoutingId(RoutingId routingId) {
        node.setRoutingId(routingId);
    }

    @Override
    public void start() {
        node.start();
    }

    @Override
    public long connectPeer(String endpoint) {
        long intentId = node.connectPeer(endpoint);
        connectionIntentIds.add(intentId);
        untypedConnectionIntents.add(intentId);
        updateSpotPeerCatalog();
        return intentId;
    }

    @Override
    public long connectPeer(String endpoint, RoutingId expectedRoutingId) {
        long intentId = node.connectPeer(endpoint, expectedRoutingId);
        connectionIntentIds.add(intentId);
        expectedConnectionRids.put(intentId, expectedRoutingId);
        updateSpotPeerCatalog();
        return intentId;
    }

    @Override
    public void removePeerConnection(long connectionIntentId) {
        node.removePeerConnection(connectionIntentId);
        connectionIntentIds.remove(connectionIntentId);
        expectedConnectionRids.remove(connectionIntentId);
        untypedConnectionIntents.remove(connectionIntentId);
        updateSpotPeerCatalog();
    }

    @Override
    public MeshNodeStatus status() {
        return node.status();
    }

    @Override
    public List<MeshPeerEntry> peers() {
        return node.peers();
    }

    @Override
    public PeerChannels peerChannels(RoutingId peerRid, long lifecycleGeneration) {
        return node.peerChannels(peerRid, lifecycleGeneration);
    }

    @Override
    public java.util.Map<String, Integer> channelWeights() {
        return java.util.Map.copyOf(channelWeights);
    }

    @Override
    public MeshNodeMonitor openMonitor() {
        return node.openMonitor();
    }

    @Override
    public List<Long> connectionIntentIds() {
        return List.copyOf(connectionIntentIds);
    }

    @Override
    public void startDispatch(Consumer<ZLinkMeshDispatchRecord> receiver) {
        if (dispatchPump != null) {
            applicationReceiver = receiver;
            return;
        }
        applicationReceiver = receiver;
        dispatchPump = new ZLinkJavaMeshDispatchPump(
            new ZLinkJavaMeshDispatchSource(node),
            record -> {
                if (operations.accept(record)) {
                    return CompletableFuture.completedFuture(null);
                }
                RecordKind kind = record.receive().kind();
                if (kind == RecordKind.NODE_SEND
                    || kind == RecordKind.NODE_REQUEST
                    || kind == RecordKind.CHANNEL_SEND
                    || kind == RecordKind.CHANNEL_REQUEST) {
                    applicationReceiver.accept(record);
                    return CompletableFuture.completedFuture(null);
                }
                return serviceDispatchReceiver.apply(record);
            },
            () -> {
                ZLinkMeshApplicationReceiver current = localApplicationReceiver;
                return current == null || current.canReceiveApplication();
            },
            Executors.newSingleThreadExecutor(Thread.ofVirtual()
                .name("zlink-mesh-dispatch-", 0)
                .factory()));
        installApplicationReceiveWakeHandler();
    }

    @Override
    public synchronized void setApplicationReceiver(ZLinkMeshApplicationReceiver receiver) {
        localApplicationReceiver = java.util.Objects.requireNonNull(receiver, "receiver");
        installApplicationReceiveWakeHandler();
        if (spotNode != null) {
            spotNode.setApplicationReceiver(receiver);
        }
    }

    private synchronized void installApplicationReceiveWakeHandler() {
        ZLinkMeshApplicationReceiver receiver = localApplicationReceiver;
        if (receiver == null) {
            return;
        }
        receiver.setApplicationReceiveReadyHandler(() -> {
            ZLinkJavaMeshDispatchPump current = dispatchPump;
            if (current != null) {
                current.requestDrain(ReadyDomain.APPLICATION.value());
            }
        });
    }

    @Override
    public synchronized ZLinkInternalSpotNode spotNode() {
        if (spotNode == null) {
            spotNode = new ZLinkJavaMeshSpotNode(this);
            spotNode.setAdmissionTimeout(routerSendTimeout);
            spotNode.setAdmissionPendingCapacity(routerPendingAdmissionCapacity);
            updateSpotPeerCatalog();
            if (localApplicationReceiver != null) {
                spotNode.setApplicationReceiver(localApplicationReceiver);
            }
            if (primaryChannelName != null) {
                spotNode.channelName(primaryChannelName);
            }
        }
        return spotNode;
    }

    private void updateSpotPeerCatalog() {
        if (spotNode != null) {
            spotNode.updateOwnerPeerCatalog(
                expectedConnectionRids.values(), !untypedConnectionIntents.isEmpty());
        }
    }

    void startServiceDispatch(
        java.util.function.Function<ZLinkMeshDispatchRecord, CompletionStage<Void>> receiver) {
        serviceDispatchReceiver = receiver;
        if (dispatchPump == null) {
            startDispatch(applicationReceiver);
        }
    }

    MeshNode nativeNode() {
        return node;
    }

    CompletionStage<Void> track(OperationId operationId) {
        if (dispatchPump == null) {
            startDispatch(ZLinkMeshDispatchRecord::close);
        }
        return operations.track(operationId);
    }

    long spotGeneration(RoutingId targetNodeRid, String targetSpotId) {
        if (!node.getRoutingId().equals(targetNodeRid) || spotNode == null) {
            return 0L;
        }
        return spotNode.spotGeneration(targetSpotId);
    }

    void actorControl(ActorControlRecord control) {
        if (spotNode != null) {
            spotNode.actorControl(control);
        }
    }

    CompletionStage<ZLinkMeshDispatchRecord> trackCompletion(OperationId operationId) {
        if (dispatchPump == null) {
            startDispatch(ZLinkMeshDispatchRecord::close);
        }
        return operations.trackCompletion(operationId);
    }

    long retainActorReply(long requestId, ReplyToken replyToken) {
        actorReplies.put(requestId, replyToken);
        return requestId;
    }

    void replyActor(long requestId, List<Message> parts) {
        ReplyToken replyToken = actorReplies.remove(requestId);
        if (replyToken == null) {
            throw new IllegalStateException("actor request reply token is no longer available");
        }
        Dispatch.reply(replyToken, parts, SendFlags.NONE);
    }

    @Override
    public void close() {
        if (dispatchPump != null) {
            dispatchPump.close();
            dispatchPump = null;
        }
        operations.close();
        actorReplies.clear();
        node.close();
    }
}
