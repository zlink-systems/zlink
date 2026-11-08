package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import systems.zlink.contracts.core.*;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.service.*;

import java.time.Duration;
import java.util.*;
import java.util.concurrent.*;

public final class ZLinkSessionFenceTestHost implements AutoCloseable {
    private final java.util.concurrent.atomic.AtomicLong nanos =
            new java.util.concurrent.atomic.AtomicLong();
    private final Context context = Zlink.createContext();
    private final ZLinkJavaRawMeshNode actorNode;
    private final ZLinkJavaRawMeshNode sessionNode;
    private final ZLinkJavaRawSpotNode actors;
    private final ZLinkJavaRawSpotNode sessions;
    private final ZLinkJavaStreamSocket stream;
    private final ZLinkBackendActorRef actor;
    private final ZLinkServiceM6BWireCodec.SessionOwnerFence session;
    private final long authority;
    private final ZLinkInternalSpotNode.BoundSessionRoute original;

    public ZLinkSessionFenceTestHost(RoutingId rid, ZLinkAuthoritySnapshot snapshot) {
        authority = snapshot.authorityOwnerGeneration();
        var actorDescriptor = descriptor(rid, 1);
        actorNode = node(actorDescriptor, null);
        sessionNode = node(descriptor(RoutingId.from("session-node"), 1), actorDescriptor);
        actors = (ZLinkJavaRawSpotNode) actorNode.spotNode();
        sessions = (ZLinkJavaRawSpotNode) sessionNode.spotNode();
        try (Message message = Message.from("create")) {
            actor = actors.createActor("actor", snapshot.objectGeneration(), message);
        }
        actors.rememberActorAuthority(actor, authority, snapshot.ownerLeaseGeneration());
        sessions.observeActorAuthority(actor, authority, snapshot.ownerLeaseGeneration());
        sessionNode
                .observePeerAdmissionExpectation(
                        actor.nodeRid(),
                        "inproc://" + actor.nodeRid(),
                        1,
                        "default",
                        ZLinkMeshNodeObjectRole.NONE)
                .toCompletableFuture()
                .join();
        actorNode
                .observePeerAdmissionExpectation(
                        sessionNode.routingId(),
                        "inproc://session-node",
                        1,
                        "default",
                        ZLinkMeshNodeObjectRole.NONE)
                .toCompletableFuture()
                .join();
        session =
                new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                        sessionNode.routingId(), 1, "owner", 1, RoutingId.from("session"), 7);
        actors.installRelocatingActorBoundSession(
                        actorFence(snapshot.ownerLeaseGeneration()), session)
                .toCompletableFuture()
                .join();
        original = actors.boundSessionRoute(actor).orElseThrow();
        stream = new ZLinkJavaStreamSocket(context.createStreamSocket(), sessionNode);
        sessions.publishStreamSession(session.sessionRid(), actor, 7, 1, stream, authority);
        assertTrue(relay(11, snapshot.ownerLeaseGeneration(), 7, actor.generation(), 1));
    }

    private ZLinkJavaRawMeshNode node(
            ZLinkServiceNodeDescriptor local, ZLinkServiceNodeDescriptor peer) {
        var lane = new ZLinkStateLane(Runnable::run);
        var topology = new ZLinkServiceTopologyRegistry(local, lane);
        if (peer != null)
            topology.admit(
                    peer,
                    "manual",
                    systems.zlink.framework.runtime.protocol.ServiceWireConstants.COMMAND_ADMIT);
        var node =
                new ZLinkJavaRawMeshNode(
                        "game",
                        () -> 0L,
                        nanos::get,
                        Duration.ofSeconds(1),
                        Duration.ofSeconds(5),
                        new ZLinkJavaRawServicePort(context),
                        Runnable::run,
                        null,
                        local,
                        topology,
                        Map.of());
        node.setRoutingId(local.nodeRoutingId());
        return node;
    }

    private static ZLinkServiceNodeDescriptor descriptor(RoutingId rid, long lifecycle) {
        return new ZLinkServiceNodeDescriptor(
                "game",
                rid,
                lifecycle,
                1,
                "inproc://" + rid,
                List.of(),
                ZLinkServiceNodeDescriptor.State.SERVING,
                "default",
                1,
                List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
                ZLinkServiceNodeDescriptor.ObjectRole.NONE,
                1,
                100,
                100,
                0,
                0);
    }

    private ZLinkServiceM6BWireCodec.ActorRouteFence actorFence(long lease) {
        return new ZLinkServiceM6BWireCodec.ActorRouteFence(actor, 1, authority, lease);
    }

    public void recoverActorLease(long lease) {
        actors.rememberActorAuthority(actor, authority, lease);
    }

    public ZLinkInternalSpotNode.BoundSessionRoute replaceActorBinding() {
        actors.retireDepartedActorBinding(actor, session.sessionRid(), authority)
                .toCompletableFuture()
                .join();
        var replacement =
                new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                        RoutingId.from("other-session-owner"),
                        2,
                        "other",
                        5,
                        RoutingId.from("replacement"),
                        8);
        actors.installRelocatingActorBoundSession(
                        actorFence(actors.actorAuthorityOwnerLeaseGeneration(actor)), replacement)
                .toCompletableFuture()
                .join();
        return sessionRoute();
    }

    public void assertLeases(long actorLease, long sessionLease) {
        var expected =
                new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                        session.nodeRid(),
                        session.nodeGeneration(),
                        session.ownerId(),
                        sessionLease,
                        session.sessionRid(),
                        session.bindingGeneration());
        actors.installRelocatingActorBoundSession(actorFence(actorLease), expected)
                .toCompletableFuture()
                .join();
    }

    public void assertActorRecoveryKeepsBindingAndSequence(long lease) {
        actors.rememberActorAuthority(actor, authority, lease);
        actors.installRelocatingActorBoundSession(actorFence(lease), session)
                .toCompletableFuture()
                .join();
        assertEquals(original, actors.boundSessionRoute(actor).orElseThrow());
        assertIdentityAndSequence();
    }

    public static List<ZLinkAutoConnectPeer> rows(List<ZLinkMeshNodeDescriptor> descriptors) {
        return descriptors.stream()
                .map(
                        d ->
                                new ZLinkAutoConnectPeer(
                                        ZLinkAutoConnectType.ROUTE_MESH,
                                        d.meshName(),
                                        d.rid(),
                                        ZLinkLocationRole.ROUTER,
                                        d.endpoint(),
                                        1,
                                        false,
                                        d.lifecycleGeneration(),
                                        Map.of(),
                                        List.of(),
                                        d.ownerId(),
                                        d.leaseGeneration(),
                                        d.updatedAt()))
                .toList();
    }

    public void poll(
            List<ZLinkMeshNodeDescriptor> descriptors, ZLinkLocationRepository repository) {
        pollAsync(descriptors, repository).toCompletableFuture().join();
    }

    public long actorLease() {
        return sessions.actorAuthorityOwnerLeaseGeneration(actor);
    }

    public ZLinkInternalMeshNode sessionMeshNode() {
        return sessionNode;
    }

    public boolean controlAllowed() {
        return sessionNode.sessionFenceCurrent(
                actor.nodeRid(), 1, "owner", sessions.actorAuthorityOwnerLeaseGeneration(actor));
    }

    public void assertIdentityAndSequence() {
        long lease = actors.actorAuthorityOwnerLeaseGeneration(actor);
        assertFalse(
                relay(11, lease, 7, actor.generation(), 1),
                "lease refresh must retain the accepted Session sequence");
        assertTrue(relay(12, lease, 7, actor.generation(), 1));
        assertFalse(relay(13, lease, 8, actor.generation(), 1));
        assertFalse(relay(13, lease, 7, actor.generation() + 1, 1));
        assertFalse(relay(13, lease, 7, actor.generation(), 2));
    }

    private boolean relay(
            long sequence, long lease, long generation, long objectGeneration, long lifecycle) {
        var result = new CompletableFuture<Boolean>();
        actors.entrySpot()
                .onDispatchEvent(
                        info -> {
                            if (info.event() == ZLinkBackendSpotDispatchEvent.ACTOR_READABLE) {
                                info.actorMessages().forEach(ZLinkBackendActorReceived::close);
                                result.complete(true);
                            }
                        });
        var target = new ZLinkBackendActorRef(actor.nodeRid(), actor.actorId(), objectGeneration);
        var header =
                new ZLinkServiceM6BWireCodec.ActorMessage(
                        true,
                        systems.zlink.framework.runtime.protocol.ServiceWireConstants
                                .FLAG_BOUND_SESSION,
                        sequence,
                        null,
                        new ZLinkServiceM6BWireCodec.ActorRouteFence(target, 1, authority, lease),
                        new ZLinkServiceM6BWireCodec.BoundSessionTail(
                                session.sessionRid(), generation, sequence));
        var message = Message.from("relay");
        boolean accepted =
                actors.enqueueRemoteActor(
                        new ZLinkInternalMeshNode.PeerAuthorityFence(
                                session.nodeRid(),
                                lifecycle,
                                session.ownerId(),
                                sessionRoute().sessionOwnerLeaseGeneration()),
                        header,
                        () -> new byte[0],
                        List.of(message),
                        null,
                        ignored -> result.complete(true),
                        failure -> result.complete(false));
        if (!accepted) {
            message.close();
            result.complete(false);
        }
        return result.orTimeout(5, TimeUnit.SECONDS).join();
    }

    public void pollSessionOwner(
            List<ZLinkMeshNodeDescriptor> descriptors, ZLinkLocationRepository repository) {
        pollSessionOwnerAsync(descriptors, repository).toCompletableFuture().join();
    }

    public CompletionStage<Void> pollSessionOwnerAsync(
            List<ZLinkMeshNodeDescriptor> descriptors, ZLinkLocationRepository repository) {
        return actorNode.refreshSessionFences(rows(descriptors), repository);
    }

    public void advanceNanos(long value) {
        nanos.set(value);
    }

    public void assertReplacementHeld() {
        var changes = new java.util.concurrent.atomic.AtomicInteger();
        actors.setRemoteSessionBindingOwner(
                (node, ref, transition) ->
                        actors.stateLane
                                .runNowOrQueue(
                                        () ->
                                                transition.apply(
                                                        new ZLinkInternalSpotNode
                                                                .RemoteSessionBindingTransition() {
                                                            public void bind(
                                                                    ZLinkInternalSpotNode
                                                                                    .BoundSessionRoute
                                                                            route) {
                                                                changes.incrementAndGet();
                                                            }

                                                            public void retire(
                                                                    ZLinkInternalSpotNode
                                                                                    .BoundSessionRoute
                                                                            route) {
                                                                changes.incrementAndGet();
                                                            }
                                                        }))
                                .thenCompose(stage -> stage));
        boolean accepted =
                actors.applyRemoteStreamBinding(
                                session.nodeRid(),
                                1,
                                session.ownerId(),
                                session.ownerLeaseGeneration(),
                                new ZLinkServiceM6BWireCodec.BoundSessionBind(
                                        17,
                                        actorFence(
                                                actors.actorAuthorityOwnerLeaseGeneration(actor)),
                                        RoutingId.from("replacement"),
                                        true,
                                        8),
                                reply -> {})
                        .toCompletableFuture()
                        .join();
        assertFalse(accepted);
        assertEquals(0, changes.get());
        assertEquals(session.sessionRid(), sessionRoute().sessionRid());
        assertEquals(7, sessionRoute().bindingGeneration());
    }

    public void assertControlHeld() {
        var failure =
                assertThrows(
                        CompletionException.class,
                        () ->
                                actors.closeActorBoundSessionAsync(actor, Duration.ofSeconds(1))
                                        .toCompletableFuture()
                                        .join());
        assertEquals(
                systems.zlink.framework.errors.ZLinkFrameworkErrorKind.UNAVAILABLE,
                assertInstanceOf(
                                systems.zlink.framework.errors.ZLinkFrameworkException.class,
                                failure.getCause())
                        .kind());
    }

    public ZLinkInternalSpotNode.BoundSessionRoute sessionRoute() {
        return actors.boundSessionRoute(actor).orElseThrow();
    }

    public void pollSessionOwnerRows(
            List<ZLinkAutoConnectPeer> rows, ZLinkLocationRepository repository) {
        actorNode.refreshSessionFences(rows, repository).toCompletableFuture().join();
    }

    public boolean actorBindingPresent() {
        return actors.hasRemoteActorBoundSessionRoute(actor);
    }

    public ZLinkBackendActorRef addRemoteActor(String id, ZLinkAuthoritySnapshot snapshot) {
        var remote = new ZLinkBackendActorRef(actor.nodeRid(), id, snapshot.objectGeneration());
        sessions.observeActorAuthority(
                remote, snapshot.authorityOwnerGeneration(), snapshot.ownerLeaseGeneration());
        sessions.publishStreamSession(
                session.sessionRid(), remote, 8, 2, stream, snapshot.authorityOwnerGeneration());
        return remote;
    }

    public long actorLease(ZLinkBackendActorRef remote) {
        return sessions.actorAuthorityOwnerLeaseGeneration(remote);
    }

    public boolean streamRoutePresent() {
        return sessions.hasLocalActorBoundSessionRoute(actor);
    }

    public CompletionStage<Void> pollAsync(
            List<ZLinkMeshNodeDescriptor> descriptors, ZLinkLocationRepository repository) {
        return sessionNode.refreshSessionFences(rows(descriptors), repository);
    }

    public void replaceStreamBinding() {
        sessions.publishStreamSession(session.sessionRid(), actor, 8, 1, stream, authority);
    }

    public void close() {
        stream.close();
        actorNode.close();
        sessionNode.close();
        context.close();
    }
}
