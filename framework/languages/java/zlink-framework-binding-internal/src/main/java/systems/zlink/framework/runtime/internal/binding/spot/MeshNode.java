/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.framework.runtime.internal.binding.spot;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.SendFlags;
import java.time.Duration;
import java.util.List;
import java.util.Optional;

/**
 * A RouteMesh membership node: peers, node/channel messaging, actors, spots,
 * publishers and the pull-based dispatch surface.
 */
public interface MeshNode extends AutoCloseable {
    /** The result of {@link #getOrCreateSpot}. */
    record SpotGetOrCreateResult(Spot spot, boolean created) {
    }

    /** Sets the node's local bind endpoint. */
    void setBind(String endpoint);

    /** Sets the node routing identity before the node starts. */
    void setRoutingId(RoutingId routingId);

    /** Returns the configured or assigned node routing identity. */
    RoutingId getRoutingId();

    /** Adds a channel by name. */
    void addChannel(String channelName);

    /** Sets the weight of a channel. */
    void setChannelWeight(String channelName, int weight);

    /** Returns the maximum accepted message size, or {@code -1} when unlimited. */
    long maxMessageSize();

    /** Sets the maximum accepted message size, or {@code -1} for unlimited. */
    void setMaxMessageSize(long value);

    /** Returns the routed admission queue high-water mark override. */
    int routerHighWaterMark();

    /** Sets the routed admission queue high-water mark override. */
    void setRouterHighWaterMark(long value);

    /** Returns the per-owner mailbox message budget. */
    long mailboxMessageBudget();

    /** Sets the per-owner mailbox message budget. */
    void setMailboxMessageBudget(long value);

    /** Starts the node. */
    void start();

    /** Shuts the node down, waiting up to the given timeout. */
    void shutdown(Duration timeout);

    /** Shuts the node down with the default timeout. */
    void shutdown();

    /** Returns a status snapshot. */
    MeshNodeStatus status();

    /** Returns the node's peer entries. */
    List<MeshPeerEntry> peers();

    /** Returns the channels advertised by one admitted peer generation. */
    PeerChannels peerChannels(RoutingId peerRid, long lifecycleGeneration);

    /** Opens the push monitor for this node. */
    MeshNodeMonitor openMonitor(long events);

    /** Opens a monitor for every MeshNode event. */
    default MeshNodeMonitor openMonitor() {
        return openMonitor(MeshMonitorEventMask.ALL);
    }

    /** Connects to a peer endpoint; returns the connection intent id. */
    long connectPeer(String endpoint);

    /** Connects to a peer endpoint with an expected routing id. */
    long connectPeer(String endpoint, RoutingId expectedRid);

    /** Removes a peer connection intent. */
    void removePeerConnection(long connectionIntentId);

    /** Disconnects an admitted peer. */
    void disconnectPeer(RoutingId peerRid, long lifecycleGeneration);

    /** Sends a message to a node. */
    void sendToNode(RoutingId targetRid, List<Message> parts, SendFlags flags);

    /** Sends a message with canonical application metadata to a node. */
    void sendToNode(RoutingId targetRid, byte[] metadata, List<Message> parts, SendFlags flags);

    /** Sends a request to a node. */
    OperationId requestToNode(RoutingId targetRid, List<Message> parts, SendFlags flags,
                              Duration timeout);

    /** Sends a request with canonical application metadata to a node. */
    OperationId requestToNode(RoutingId targetRid, byte[] metadata, List<Message> parts,
                              SendFlags flags, Duration timeout);

    /** Sends a message to a channel. */
    void sendToChannel(String channelName, List<Message> parts, SendFlags flags);

    /** Sends a message with canonical application metadata to a channel. */
    void sendToChannel(String channelName, byte[] metadata, List<Message> parts, SendFlags flags);

    /** Sends a request to a channel. */
    OperationId requestToChannel(String channelName, List<Message> parts, SendFlags flags,
                                 Duration timeout);

    /** Sends a request with canonical application metadata to a channel. */
    OperationId requestToChannel(String channelName, byte[] metadata, List<Message> parts,
                                 SendFlags flags, Duration timeout);

    /** Sends a message to an actor. */
    void sendToActor(ActorRef actor, List<Message> parts, SendFlags flags);

    /** Sends a request to an actor. */
    OperationId requestToActor(ActorRef actor, List<Message> parts, SendFlags flags,
                               Duration timeout);

    /** Sends a message to an actor's bound STREAM session. */
    void sendActorBoundSession(ActorRef actor, List<Message> parts, SendFlags flags);

    /** Creates an actor with no creation parts. */
    Actor createActor(String actorId);

    /** Creates an actor with the given creation parts. */
    Actor createActor(String actorId, List<Message> creationParts);

    /** Looks up an actor hosted locally. */
    Optional<ActorLocation> actorLookup(String actorId);

    /** Looks up an actor on a remote node. */
    OperationId actorLookupRemote(RoutingId targetNodeRid, String actorId, Duration timeout);

    /** Destroys an actor. */
    OperationId destroyActor(ActorRef actor, Duration timeout);

    /** Joins an actor to a user spot. */
    OperationId joinActorSpot(
        ActorRef actor,
        RoutingId targetNodeRid,
        RoutingId targetSpotId,
        long targetSpotGeneration,
        List<Message> creationParts,
        Duration timeout);

    /** Joins an actor to a node's entry spot. */
    OperationId joinActorEntrySpot(
        ActorRef actor,
        RoutingId targetNodeRid,
        List<Message> creationParts,
        Duration timeout);

    /** Leaves the actor's current spot at the expected membership epoch. */
    OperationId leaveActor(
        ActorRef actor,
        long expectedMembershipEpoch,
        Duration timeout);

    /** Closes the actor's bound STREAM session. */
    OperationId closeActorBoundSession(
        ActorRef actor,
        long expectedBindingGeneration,
        Duration timeout);

    /**
     * Fences one side of an actor transfer and returns its opaque token and
     * negotiated reservation.
     */
    PrepareActorTransferResult prepareActorTransfer(
        ActorTransferPrepare prepare, Duration timeout);

    /** Commits a prepared actor transfer to the given membership epoch. */
    void commitActorTransfer(ActorTransferToken token, long newMembershipEpoch);

    /** Activates a committed target actor transfer. */
    void activateActorTransfer(ActorTransferToken token);

    /** Aborts a prepared or committed actor transfer. */
    void abortActorTransfer(ActorTransferToken token);

    /** Creates a new user spot. */
    Spot createSpot();

    /** Returns the node's entry spot. */
    Spot entrySpot();

    /** Gets or creates a spot with the given routing id. */
    SpotGetOrCreateResult getOrCreateSpot(String spotId);

    /** Looks up an existing spot by routing id. */
    Optional<Spot> spotLookup(String spotId);

    /** Creates a node-level publisher. */
    MeshNodePublisher createPublisher();

    /** Installs (or clears, when null) the dispatch ready handler. */
    void setReadyHandler(MeshReadyHandler handler);

    /** Drains ready owners into the given batch. */
    DrainResult drainReady(int domains, ReadyBatch batch, RecvFlags flags);

    @Override
    void close();

    /** Builds an unchecked remote actor reference (generation 0). */
    static ActorRef remoteActorRef(RoutingId targetNodeRid, String actorId) {
        return new ActorRef(targetNodeRid, actorId, 0L);
    }
}
