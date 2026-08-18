package systems.zlink.framework.runtime.binding;
import java.nio.ByteBuffer;
import java.util.concurrent.ExecutionException;
import java.util.stream.IntStream;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.HexFormat;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import java.util.OptionalLong;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.contracts.eventing.MonitorEvent;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.framework.runtime.internal.binding.spot.RecordKind;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceMessageFollowWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceAdmissionGuard;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

final class ZLinkJavaRawMeshNodeM6ATest {
    @Test
    void readyPeerRequiresTheSelectedApplicationTransportPair() {
        var selected = new ZLinkJavaRawMeshNode.TransportPair(41, 7);

        assertFalse(ZLinkJavaRawMeshNode.applicationTransportPairMatches(
            selected, null));
        assertFalse(ZLinkJavaRawMeshNode.applicationTransportPairMatches(
            selected, new ZLinkJavaRawMeshNode.TransportPair(42, 7)));
        assertFalse(ZLinkJavaRawMeshNode.applicationTransportPairMatches(
            selected, new ZLinkJavaRawMeshNode.TransportPair(41, 8)));
        assertTrue(ZLinkJavaRawMeshNode.applicationTransportPairMatches(
            selected, new ZLinkJavaRawMeshNode.TransportPair(41, 7)));
        assertTrue(ZLinkJavaRawMeshNode.applicationTransportPairMatches(
            null, null));
        assertFalse(ZLinkJavaRawMeshNode.applicationTransportPairMatches(
            null, selected));
    }

    @Test
    void spotRouterNodeReadyCheckUsesTheDirectApplicationRequestPair() {
        var topologySelected = new ZLinkJavaRawMeshNode.TransportPair(73, 11);
        var directApplicationRequest =
            new ZLinkJavaRawMeshNode.TransportPair(73, 11);

        assertTrue(ZLinkJavaRawMeshNode.applicationTransportPairMatches(
            topologySelected, directApplicationRequest));
        assertFalse(ZLinkJavaRawMeshNode.applicationTransportPairMatches(
            topologySelected,
            new ZLinkJavaRawMeshNode.TransportPair(73, 12)));
    }

    @Test
    void oneWayAdapterClassifiesNativeSubmitRejection() {
        for (var rejected : List.of(
                systems.zlink.contracts.sockets.SubmitResult.BACKPRESSURED,
                systems.zlink.contracts.sockets.SubmitResult.NOT_ADMITTED,
                systems.zlink.contracts.sockets.SubmitResult.NOT_CONNECTED)) {
            var failure = assertThrows(
                ExecutionException.class,
                () -> ZLinkOneWayCalls.adaptOneWay(
                    CompletableFuture.failedFuture(
                        new systems.zlink.contracts.errors.ZlinkSubmitException(
                            rejected)))
                    .toCompletableFuture()
                    .get());
            assertTrue(failure.getCause()
                instanceof systems.zlink.framework.errors.ZLinkFrameworkException);
            var framework = (systems.zlink.framework.errors.ZLinkFrameworkException)
                failure.getCause();
            assertEquals(
                rejected == systems.zlink.contracts.sockets.SubmitResult.NOT_CONNECTED
                    ? systems.zlink.framework.errors.ZLinkFrameworkErrorKind.UNAVAILABLE
                    : systems.zlink.framework.errors.ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                framework.kind());
        }
    }

    @Test
    void monitorConnectionKeyIgnoresEventSpecificValue() {
        var ready = new MonitorEvent(
            MonitorEventType.CONNECTION_READY,
            1,
            Optional.of(RoutingId.from("peer")),
            "tcp://127.0.0.1:4000",
            "tcp://127.0.0.1:5000");
        var disconnected = new MonitorEvent(
            MonitorEventType.DISCONNECTED,
            4,
            Optional.of(RoutingId.from("peer")),
            "tcp://127.0.0.1:4000",
            "tcp://127.0.0.1:5000");

        assertEquals(
            ZLinkJavaRawMeshNode.transportEventKey(ready),
            ZLinkJavaRawMeshNode.transportEventKey(disconnected));
    }

    @Test
    void connectionIdForAdmissionReusesExistingPairAcrossDirectionBuckets()
        throws Exception {
        // Regression for the spot-route Java-client/C++-host cross-language
        // flake: HELLO and ADMIT infer OPPOSITE local "directions" purely
        // from the command (see connectionIdForAdmission), but both can
        // arrive on the exact SAME physical transport pair when a peer we
        // blind-connect to speaks first (sends HELLO) and then separately
        // confirms with its own ADMIT on that same connection. The first
        // (HELLO) admission message mints a connectionId for that pair; if
        // the second (ADMIT) message -- inferring the opposite direction --
        // is allowed to mint a SECOND connectionId for the SAME physical
        // pair, the already-admitted peer collides with itself as a false
        // duplicate and gets rejected (DUPLICATE_REJECTED), corrupting
        // admission for the rest of the session and producing a persistent
        // "peer-not-admitted" / NOT_CONNECTED on the peer's side.
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            node.setRoutingId(RoutingId.from(
                "connection-id-reuse-local-" + System.nanoTime()));
            node.setBind(
                "inproc://jvm-connection-id-reuse-" + System.nanoTime());
            node.start();
            RoutingId peer = RoutingId.from("connection-id-reuse-peer");
            var pair = new ZLinkJavaRawMeshNode.TransportPair(1, 1);

            Method connectionIdForAdmission = ZLinkJavaRawMeshNode.class
                .getDeclaredMethod(
                    "connectionIdForAdmission",
                    RoutingId.class,
                    ZLinkServiceAdmissionGuard.ConnectionDirection.class,
                    ZLinkJavaRawMeshNode.TransportPair.class);
            connectionIdForAdmission.setAccessible(true);

            // First admission message on this pair (HELLO): direction
            // inferred as INBOUND, nothing assigned to the pair yet -- mints
            // a fresh connectionId.
            String first = (String) connectionIdForAdmission.invoke(
                node,
                peer,
                ZLinkServiceAdmissionGuard.ConnectionDirection.INBOUND,
                pair);
            assertNotNull(first);

            // Second admission message on the EXACT SAME physical pair
            // (ADMIT): direction inferred as OUTBOUND -- must reuse the
            // SAME connectionId already assigned to this pair, not mint a
            // second one for one physical connection.
            String second = (String) connectionIdForAdmission.invoke(
                node,
                peer,
                ZLinkServiceAdmissionGuard.ConnectionDirection.OUTBOUND,
                pair);
            assertEquals(first, second);
        }
    }

    @Test
    void connectionIdForAdmissionReusesTheMonitorRegisteredIdOnFirstMessage()
        throws Exception {
        // Same regression, but covers the actual production ordering: the
        // CONNECTION_READY monitor edge registers a connectionId for a pair
        // (see registerTransportConnection) BEFORE any admission message is
        // ever processed for it. The very first admission message on that
        // pair must reuse that monitor-registered id rather than minting a
        // fresh one -- otherwise the monitor's id is immediately orphaned in
        // pendingConnectionIds and can resurface later (consumed by a
        // subsequent message inferring a different direction), which is the
        // exact stranding this fix closes.
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            node.setRoutingId(RoutingId.from(
                "connection-id-reuse-local-" + System.nanoTime()));
            node.setBind(
                "inproc://jvm-connection-id-reuse-monitor-"
                    + System.nanoTime());
            node.start();
            RoutingId peer = RoutingId.from(
                "connection-id-reuse-monitor-peer");
            var pair = new ZLinkJavaRawMeshNode.TransportPair(2, 1);
            String monitorRegisteredId = "monitor-registered-id";

            var transportPairsField = ZLinkJavaRawMeshNode.class
                .getDeclaredField("transportPairs");
            transportPairsField.setAccessible(true);
            @SuppressWarnings("unchecked")
            var transportPairs = (java.util.Map<String, ZLinkJavaRawMeshNode.TransportPair>)
                transportPairsField.get(node);
            transportPairs.put(monitorRegisteredId, pair);

            Method connectionIdForAdmission = ZLinkJavaRawMeshNode.class
                .getDeclaredMethod(
                    "connectionIdForAdmission",
                    RoutingId.class,
                    ZLinkServiceAdmissionGuard.ConnectionDirection.class,
                    ZLinkJavaRawMeshNode.TransportPair.class);
            connectionIdForAdmission.setAccessible(true);

            String result = (String) connectionIdForAdmission.invoke(
                node,
                peer,
                ZLinkServiceAdmissionGuard.ConnectionDirection.INBOUND,
                pair);
            assertEquals(monitorRegisteredId, result);
        }
    }

    @Test
    void sourceWideAdmissionReadySelectsOnlyReadyPeers() {
        RoutingId readyRid = RoutingId.from("ready-peer");
        RoutingId pendingRid = RoutingId.from("pending-peer");
        var ready = new ZLinkServiceTopologyRegistry.Peer(
            descriptor(readyRid),
            new ZLinkServiceTopologyRegistry.Connection(
                "ready-connection",
                ZLinkServiceAdmissionGuard.ConnectionDirection.OUTBOUND,
                "ready-discriminator"));
        var pending = new ZLinkServiceTopologyRegistry.Peer(
            descriptor(pendingRid),
            new ZLinkServiceTopologyRegistry.Connection(
                "pending-connection",
                ZLinkServiceAdmissionGuard.ConnectionDirection.OUTBOUND,
                "pending-discriminator"));

        assertEquals(
            List.of(readyRid),
            ZLinkJavaRawMeshNode.readyAdmissionPeerIds(
                List.of(ready, pending),
                peer -> peer.connectionId().equals("ready-connection")));
    }

    private static ZLinkServiceNodeDescriptor descriptor(RoutingId rid) {
        return new ZLinkServiceNodeDescriptor(
            "mesh",
            rid,
            1,
            1,
            "inproc://" + rid,
            List.of(),
            ZLinkServiceNodeDescriptor.State.SERVING,
            "test-security",
            1,
            List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
            ZLinkServiceNodeDescriptor.ObjectRole.NONE,
            1,
            1,
            0,
            0,
            0);
    }

    @Test
    void ephemeralBindPublishesTheActualListenerEndpoint() {
        try (var context = Zlink.createContext();
             var node = meshNode(context)) {
            node.setRoutingId(RoutingId.from("jvm-ephemeral-endpoint"));
            node.setBind("tcp://127.0.0.1:0");
            node.start();

            String endpoint = node.status().localEndpoint();
            assertTrue(endpoint.startsWith("tcp://127.0.0.1:"));
            assertTrue(!endpoint.endsWith(":0"), endpoint);
        }
    }

    @Test
    void manualObjectClientPairIsNotRequiredButWeightZeroServerMembershipConnects()
        throws Exception {
        RoutingId leftRid = RoutingId.from("jvm-client-left");
        RoutingId rightRid = RoutingId.from("jvm-client-right");
        String endpoint = "inproc://jvm-client-pair-" + System.nanoTime();
        try (var context = Zlink.createContext();
             var left = meshNode(context);
             var right = meshNode(context)) {
            left.setRoutingId(leftRid);
            left.setBind(endpoint);
            left.setObjectRole(ZLinkMeshNodeObjectRole.CLIENT);
            right.setRoutingId(rightRid);
            right.setBind("inproc://jvm-client-right-" + System.nanoTime());
            right.setObjectRole(ZLinkMeshNodeObjectRole.CLIENT);
            left.start();
            right.start();
            right.connectPeer(endpoint, leftRid);

            awaitState(right, MeshPeerState.NOT_REQUIRED);
            assertEquals(
                ZLinkOneWayCalls.TARGET_NOT_FOUND,
                right.spotNode().classifyNodeSendTarget(leftRid)
                    .orElseThrow());
        }

        String serverEndpoint =
            "inproc://jvm-client-server-channel-" + System.nanoTime();
        try (var context = Zlink.createContext();
             var left = meshNode(context);
             var right = meshNode(context)) {
            left.setRoutingId(leftRid);
            left.setBind(serverEndpoint);
            left.setObjectRole(ZLinkMeshNodeObjectRole.CLIENT);
            left.addChannel("weight-zero");
            left.setChannelWeight("weight-zero", 0);
            right.setRoutingId(rightRid);
            right.setBind("inproc://jvm-client-caller-" + System.nanoTime());
            right.setObjectRole(ZLinkMeshNodeObjectRole.CLIENT);
            left.start();
            right.start();
            right.connectPeer(serverEndpoint, leftRid);

            awaitState(right, MeshPeerState.ADMITTED);
        }
    }

    @Test
    void descriptorBackedObjectClientIsNotRequiredAndNodeDirectIsNotFound() {
        RoutingId localRid = RoutingId.from("jvm-auto-client-local");
        RoutingId peerRid = RoutingId.from("jvm-auto-client-peer");
        try (var context = Zlink.createContext();
             var local = meshNode(context)) {
            local.setRoutingId(localRid);
            local.setBind("inproc://jvm-auto-client-local-" + System.nanoTime());
            local.setObjectRole(ZLinkMeshNodeObjectRole.CLIENT);
            local.start();

            local.markPeerConnectionNotRequired(
                peerRid,
                "inproc://jvm-auto-client-peer",
                7);

            assertEquals(MeshPeerState.NOT_REQUIRED, local.peers().getFirst().state());
            assertEquals(
                ZLinkOneWayCalls.TARGET_NOT_FOUND,
                local.spotNode().classifyNodeSendTarget(peerRid).orElseThrow());
        }
    }

    @Test
    void bilateralManualConnectKeepsOneReadyPeer()
        throws Exception {
        RoutingId lowerRid = RoutingId.from("a");
        RoutingId higherRid = RoutingId.from("z");
        String lowerEndpoint =
            "inproc://jvm-bilateral-lower-" + System.nanoTime();
        String higherEndpoint =
            "inproc://jvm-bilateral-higher-" + System.nanoTime();
        try (var context = Zlink.createContext();
             var lower = meshNode(context);
             var higher = meshNode(context)) {
            lower.setRoutingId(lowerRid);
            lower.setBind(lowerEndpoint);
            higher.setRoutingId(higherRid);
            higher.setBind(higherEndpoint);
            lower.start();
            higher.start();

            lower.connectPeer(higherEndpoint, higherRid);
            higher.connectPeer(lowerEndpoint, lowerRid);

            awaitAdmitted(lower);
            awaitAdmitted(higher);

            assertEquals(1, lower.peers().size());
            assertEquals(1, higher.peers().size());
            assertEquals(MeshPeerState.ADMITTED, lower.peers().getFirst().state());
            assertEquals(MeshPeerState.ADMITTED, higher.peers().getFirst().state());

            CompletableFuture<ZLinkMeshDispatchRecord> received =
                new CompletableFuture<>();
            higher.startDispatch(received::complete);
            try (Message packet = Message.from("bilateral");
                 Message payload = Message.from(new byte[] {4, 2})) {
                lower.spotNode().sendToNode(
                        higherRid,
                        List.of(packet, payload))
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);
            }
            try (ZLinkMeshDispatchRecord record =
                received.get(2, TimeUnit.SECONDS)) {
                assertEquals(RecordKind.NODE_SEND, record.receive().kind());
                assertEquals(lowerRid, record.receive().sourceNodeRid());
            }
        }
    }

    @Test
    void closedExpectedPeerClassifiesDirectSendAsRouteNotConnected()
        throws Exception {
        RoutingId localRid = RoutingId.from("jvm-closed-local");
        RoutingId peerRid = RoutingId.from("jvm-closed-peer");
        String localEndpoint = "inproc://jvm-closed-local-" + System.nanoTime();
        String peerEndpoint = "inproc://jvm-closed-peer-" + System.nanoTime();
        try (var context = Zlink.createContext();
             var local = new ZLinkJavaRawMeshNode(
                 context,
                 "mesh",
                 System::currentTimeMillis,
                 Duration.ofMillis(10),
                 Duration.ofMillis(50));
             var peer = new ZLinkJavaRawMeshNode(
                 context,
                 "mesh",
                 System::currentTimeMillis,
                 Duration.ofMillis(10),
                 Duration.ofMillis(50))) {
            local.setRoutingId(localRid);
            local.setBind(localEndpoint);
            peer.setRoutingId(peerRid);
            peer.setBind(peerEndpoint);
            local.start();
            peer.start();
            local.connectPeer(peerEndpoint, peerRid);
            awaitState(local, MeshPeerState.ADMITTED);

            peer.close();
            awaitState(local, MeshPeerState.CLOSED);

            assertEquals(
                ZLinkOneWayCalls.ROUTE_NOT_CONNECTED,
                local.spotNode().classifyNodeSendTarget(peerRid).orElseThrow());
        }
    }

    @Test
    void disconnectedChannelRemainsUnavailableUntilAutoTargetIsRemoved()
        throws Exception {
        RoutingId localRid = RoutingId.from("jvm-channel-local");
        RoutingId peerRid = RoutingId.from("jvm-channel-peer");
        String localEndpoint = "inproc://jvm-channel-local-" + System.nanoTime();
        String peerEndpoint = "inproc://jvm-channel-peer-" + System.nanoTime();
        try (var context = Zlink.createContext();
             var local = new ZLinkJavaRawMeshNode(
                 context,
                 "mesh",
                 System::currentTimeMillis,
                 Duration.ofMillis(10),
                 Duration.ofMillis(50));
             var peer = new ZLinkJavaRawMeshNode(
                 context,
                 "mesh",
                 System::currentTimeMillis,
                 Duration.ofMillis(10),
                 Duration.ofMillis(50))) {
            local.setRoutingId(localRid);
            local.setBind(localEndpoint);
            peer.setRoutingId(peerRid);
            peer.setBind(peerEndpoint);
            peer.addChannel("game.api");
            peer.setChannelWeight("game.api", 100);
            local.start();
            peer.start();
            local.observePeerAdmissionExpectation(
                peerRid,
                peerEndpoint,
                peer.status().lifecycleGeneration(),
                ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY);
            long intent = local.connectPeer(peerEndpoint, peerRid);
            awaitState(local, MeshPeerState.ADMITTED);
            assertTrue(local.classifyChannelTarget("game.api").isEmpty());

            peer.close();
            awaitState(local, MeshPeerState.CLOSED);
            assertEquals(
                ZLinkOneWayCalls.ROUTE_NOT_CONNECTED,
                local.classifyChannelTarget("game.api").orElseThrow());

            local.forgetPeerAdmissionExpectation(peerRid);
            try {
                local.removePeerConnection(intent);
            } catch (RuntimeException alreadyDisconnected) {
                // The remote inproc endpoint has already closed. The intent
                // cleanup still removes its last-admitted channel knowledge.
            }
            assertEquals(
                ZLinkOneWayCalls.TARGET_NOT_FOUND,
                local.classifyChannelTarget("game.api").orElseThrow());
        }
    }

    @Test
    void descriptorBackedPeerIntentRequiresLifecycleAndSecurityFence()
        throws Exception {
        RoutingId localRid = RoutingId.from("jvm-descriptor-fence-local");
        RoutingId peerRid = RoutingId.from("jvm-descriptor-fence-peer");
        String localEndpoint = "inproc://jvm-descriptor-fence-local-"
            + System.nanoTime();
        String peerEndpoint = "inproc://jvm-descriptor-fence-peer-"
            + System.nanoTime();
        try (var context = Zlink.createContext();
             var local = meshNode(context);
             var peer = meshNode(context)) {
            local.setRoutingId(localRid);
            local.setBind(localEndpoint);
            peer.setRoutingId(peerRid);
            peer.setBind(peerEndpoint);
            local.start();
            peer.start();

            long wrongGenerationIntent = local.connectPeer(
                peerEndpoint,
                peerRid,
                peer.status().lifecycleGeneration() + 1,
                ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY);
            awaitState(local, MeshPeerState.ERROR);
            assertFalse(local.peers().stream().anyMatch(peerEntry ->
                peerEntry.state() == MeshPeerState.ADMITTED));
            try {
                local.removePeerConnection(wrongGenerationIntent);
            } catch (RuntimeException alreadyRejected) {
                // Admission rejection may close the transport before cleanup.
            }

            long wrongSecurityIntent = local.connectPeer(
                peerEndpoint,
                peerRid,
                peer.status().lifecycleGeneration(),
                "wrong-security-identity");
            awaitState(local, MeshPeerState.ERROR);
            assertFalse(local.peers().stream().anyMatch(peerEntry ->
                peerEntry.state() == MeshPeerState.ADMITTED));
            try {
                local.removePeerConnection(wrongSecurityIntent);
            } catch (RuntimeException alreadyRejected) {
                // Admission rejection may close the transport before cleanup.
            }

            local.connectPeer(
                peerEndpoint,
                peerRid,
                peer.status().lifecycleGeneration(),
                ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY);
            awaitState(local, MeshPeerState.ADMITTED);
        }
    }

    @Test
    void descriptorFenceReplacesEndpointOnlyIntent() throws Exception {
        RoutingId localRid = RoutingId.from("jvm-descriptor-replace-local");
        RoutingId peerRid = RoutingId.from("jvm-descriptor-replace-peer");
        String localEndpoint = "inproc://jvm-descriptor-replace-local-"
            + System.nanoTime();
        String peerEndpoint = "inproc://jvm-descriptor-replace-peer-"
            + System.nanoTime();
        try (var context = Zlink.createContext();
             var local = meshNode(context);
             var peer = meshNode(context)) {
            local.setRoutingId(localRid);
            local.setBind(localEndpoint);
            peer.setRoutingId(peerRid);
            peer.setBind(peerEndpoint);
            local.start();
            peer.start();

            local.connectPeer(peerEndpoint);
            awaitTransport(local, peerEndpoint);
            assertThrows(
                IllegalStateException.class,
                () -> local.replacePeerConnection(
                    peerEndpoint,
                    peerRid,
                    peer.status().lifecycleGeneration(),
                    ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY));

            peer.close();
            try (var replacementPeer = new ZLinkJavaRawMeshNode(
                     context, "mesh")) {
                replacementPeer.setRoutingId(peerRid);
                replacementPeer.setBind(peerEndpoint);
                replacementPeer.start();
                long fencedIntent = awaitReplacement(
                    local,
                    peerEndpoint,
                    peerRid,
                    replacementPeer.status().lifecycleGeneration(),
                    ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY);

                awaitState(local, MeshPeerState.ADMITTED);
                var admitted = local.peers().stream()
                    .filter(entry -> entry.state() == MeshPeerState.ADMITTED)
                    .findFirst()
                    .orElseThrow();
                assertEquals(fencedIntent, admitted.connectionIntentId());
                assertEquals(
                    replacementPeer.status().lifecycleGeneration(),
                    admitted.lifecycleGeneration());
                assertThrows(
                    IllegalStateException.class,
                    () -> local.replacePeerConnection(
                        peerEndpoint,
                        peerRid,
                        replacementPeer.status().lifecycleGeneration() + 1,
                        ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY));
            }
        }
    }

    @Test
    void replacementDoesNotSkipAConnectionBeforeItsReadyEvent() throws Exception {
        RoutingId localRid = RoutingId.from("jvm-pre-ready-replace-local");
        RoutingId peerRid = RoutingId.from("jvm-pre-ready-replace-peer");
        String localEndpoint = "inproc://jvm-pre-ready-replace-local-"
            + System.nanoTime();
        String peerEndpoint = "inproc://jvm-pre-ready-replace-peer-"
            + System.nanoTime();
        try (var context = Zlink.createContext();
             var local = meshNode(context);
             var peer = meshNode(context)) {
            local.setRoutingId(localRid);
            local.setBind(localEndpoint);
            peer.setRoutingId(peerRid);
            peer.setBind(peerEndpoint);
            local.start();
            peer.start();

            local.connectPeer(peerEndpoint);
            assertThrows(
                IllegalStateException.class,
                () -> local.replacePeerConnection(
                    peerEndpoint,
                    peerRid,
                    peer.status().lifecycleGeneration(),
                    ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY));

            peer.close();
            try (var replacementPeer = new ZLinkJavaRawMeshNode(
                     context, "mesh")) {
                replacementPeer.setRoutingId(peerRid);
                replacementPeer.setBind(peerEndpoint);
                replacementPeer.start();
                long replacementIntent = awaitReplacement(
                    local,
                    peerEndpoint,
                    peerRid,
                    replacementPeer.status().lifecycleGeneration(),
                    ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY);

                awaitState(local, MeshPeerState.ADMITTED);
                assertEquals(
                    replacementIntent,
                    local.peers().stream()
                        .filter(entry -> entry.state() == MeshPeerState.ADMITTED)
                        .findFirst()
                        .orElseThrow()
                        .connectionIntentId());
            }
        }
    }

    @Test
    void command42ReceivesCommand43ThroughInfrastructureDispatcher()
        throws Exception {
        String endpoint = "inproc://jvm-m6c-session-seal-" + System.nanoTime();
        RoutingId sourceRid = RoutingId.from("jvm-m6c-seal-source");
        RoutingId sessionOwnerRid = RoutingId.from("jvm-m6c-seal-owner");
        RoutingId sessionRid = RoutingId.from("jvm-m6c-seal-session");
        var codec = new ZLinkServiceM6BWireCodec();
        var seal = new ZLinkServiceM6BWireCodec.SessionRelocationSeal(
            new ZLinkServiceM6BWireCodec.RelocationIdentity(1, 2),
            new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                "coordinator", 3, sourceRid, 4, "store-v5"),
            ZLinkServiceM6BWireCodec.RelocationRole.SOURCE,
            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                new systems.zlink.framework.runtime.internal.backend
                    .ZLinkBackendActorRef(sourceRid, "actor", 6),
                4, 10, 11),
            new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                sessionOwnerRid, 7, "session-owner", 8, sessionRid, 9));
        try (var context = Zlink.createContext();
             var source = meshNode(context);
             var sessionOwner = meshNode(context)) {
            source.setRoutingId(sourceRid);
            source.setBind("inproc://jvm-m6c-seal-source-" + System.nanoTime());
            sessionOwner.setRoutingId(sessionOwnerRid);
            sessionOwner.setBind(endpoint);
            AtomicInteger applicationDispatches = new AtomicInteger();
            sessionOwner.startDispatch(record -> {
                applicationDispatches.incrementAndGet();
                record.close();
            });
            sessionOwner.setSessionRelocationSealHandler((actualSource, encoded) -> {
                assertEquals(sourceRid, actualSource);
                assertEquals(seal, codec.decodeSessionRelocationSeal(encoded));
                return CompletableFuture.completedFuture(
                    codec.encodeSessionRelocationSealed(
                        new ZLinkServiceM6BWireCodec.SessionRelocationSealed(
                            seal.relocation(), seal.coordinator(),
                            seal.actor(), seal.session())));
            });
            source.start();
            sessionOwner.start();
            source.connectPeer(endpoint, sessionOwnerRid);
            awaitAdmitted(source);

            var sealed = codec.decodeSessionRelocationSealed(
                source.requestSessionRelocationSeal(
                        sessionOwnerRid,
                        codec.encodeSessionRelocationSeal(seal),
                        Duration.ofSeconds(2))
                    .toCompletableFuture().get(2, TimeUnit.SECONDS));

            assertEquals(seal.relocation(), sealed.relocation());
            assertEquals(seal.session(), sealed.session());
            assertEquals(0, applicationDispatches.get());
        }
    }

    @Test
    void command44UsesOneWayInfrastructureDispatch()
        throws Exception {
        String endpoint = "inproc://jvm-m6c-session-route-" + System.nanoTime();
        RoutingId sourceRid = RoutingId.from("jvm-m6c-target-owner");
        RoutingId sessionOwnerRid = RoutingId.from("jvm-m6c-session-owner");
        RoutingId sessionRid = RoutingId.from("jvm-m6c-session");
        var codec = new ZLinkServiceM6BWireCodec();
        var command = new ZLinkServiceM6BWireCodec.SessionRelocationRoute(
            new ZLinkServiceM6BWireCodec.RelocationIdentity(1, 2),
            new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                "coordinator", 3, sourceRid, 4, "store-v5"),
            ZLinkServiceM6BWireCodec.RelocationRole.TARGET,
            new ZLinkServiceM6BWireCodec.ActorIdentity("actor", 6),
            new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                sessionOwnerRid, 7, "session-owner", 8, sessionRid, 9),
            ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.COMMIT,
            10, 11, sourceRid, 12);
        try (var context = Zlink.createContext();
             var source = meshNode(context);
             var sessionOwner = meshNode(context)) {
            source.setRoutingId(sourceRid);
            source.setBind("inproc://jvm-m6c-target-owner-" + System.nanoTime());
            sessionOwner.setRoutingId(sessionOwnerRid);
            sessionOwner.setBind(endpoint);
            AtomicInteger applicationDispatches = new AtomicInteger();
            sessionOwner.startDispatch(record -> {
                applicationDispatches.incrementAndGet();
                record.close();
            });
            sessionOwner.setSessionRelocationRouteHandler((actualSource, encoded) -> {
                assertEquals(sourceRid, actualSource);
                assertEquals(command, codec.decodeSessionRelocationRoute(encoded));
                return CompletableFuture.completedFuture(null);
            });
            source.start();
            sessionOwner.start();
            source.connectPeer(endpoint, sessionOwnerRid);
            awaitAdmitted(source);

            source.sendSessionRelocationRoute(
                    sessionOwnerRid,
                    codec.encodeSessionRelocationRoute(command))
                .toCompletableFuture().get(2, TimeUnit.SECONDS);
            assertEquals(0, applicationDispatches.get());
        }
    }

    @Test
    void relocationControlUsesAdmittedPeerAndBypassesApplicationMailbox()
        throws Exception {
        String endpoint = "inproc://jvm-m6c-relocation-" + System.nanoTime();
        RoutingId sourceRid = RoutingId.from("jvm-m6c-relocation-source");
        RoutingId targetRid = RoutingId.from("jvm-m6c-relocation-target");
        try (var context = Zlink.createContext();
             var source = meshNode(context);
             var target = meshNode(context)) {
            source.setRoutingId(sourceRid);
            source.setBind("inproc://jvm-m6c-relocation-source-"
                + System.nanoTime());
            target.setRoutingId(targetRid);
            target.setBind(endpoint);
            AtomicInteger applicationDispatches = new AtomicInteger();
            target.startDispatch(record -> {
                applicationDispatches.incrementAndGet();
                record.close();
            });
            target.setRelocationControlHandler((actualSource, command) -> {
                assertEquals(sourceRid, actualSource);
                assertArrayEquals(new byte[] {1, 2, 3}, command);
                return CompletableFuture.completedFuture(
                    new byte[] {9, 8, 7});
            });
            source.start();
            target.start();
            source.connectPeer(endpoint, targetRid);
            awaitAdmitted(source);

            byte[] reply = source.requestRelocationControl(
                    targetRid,
                    new byte[] {1, 2, 3},
                    Duration.ofSeconds(2))
                .toCompletableFuture()
                .get(2, TimeUnit.SECONDS);

            assertArrayEquals(new byte[] {9, 8, 7}, reply);
            assertEquals(0, applicationDispatches.get());
        }
    }

    @Test
    void canonicalRelocationControlUsesRawInfrastructureLane()
        throws Exception {
        String endpoint =
            "inproc://jvm-m6c-canonical-relocation-" + System.nanoTime();
        RoutingId sourceRid =
            RoutingId.from("jvm-m6c-canonical-source");
        RoutingId targetRid =
            RoutingId.from("jvm-m6c-canonical-target");
        byte[] command30 = canonicalRelocationCommand(30);
        try (var context = Zlink.createContext();
             var source = meshNode(context);
             var target = meshNode(context)) {
            source.setRoutingId(sourceRid);
            source.setBind(
                "inproc://jvm-m6c-canonical-source-" + System.nanoTime());
            target.setRoutingId(targetRid);
            target.setBind(endpoint);
            AtomicInteger applicationDispatches = new AtomicInteger();
            target.startDispatch(record -> {
                applicationDispatches.incrementAndGet();
                record.close();
            });
            CompletableFuture<byte[]> received = new CompletableFuture<>();
            target.setCanonicalRelocationControlHandler(
                (actualSource, command) -> {
                    assertEquals(sourceRid, actualSource);
                    received.complete(command);
                    return CompletableFuture.completedFuture(null);
                });
            source.start();
            target.start();
            source.connectPeer(endpoint, targetRid);
            awaitAdmitted(source);

            source.sendCanonicalRelocationControl(targetRid, command30)
                .toCompletableFuture()
                .get(2, TimeUnit.SECONDS);

            assertArrayEquals(
                command30,
                received.get(2, TimeUnit.SECONDS));
            assertEquals(0, applicationDispatches.get());
        }
    }

    @Test
    void infrastructureControlProgressesWhileApplicationDispatchIsBlocked()
        throws Exception {
        String endpoint =
            "inproc://jvm-infrastructure-control-" + System.nanoTime();
        RoutingId sourceRid = RoutingId.from("jvm-infrastructure-source");
        RoutingId targetRid = RoutingId.from("jvm-infrastructure-target");
        byte[] command30 = canonicalRelocationCommand(30);
        CountDownLatch applicationEntered = new CountDownLatch(1);
        CountDownLatch releaseApplication = new CountDownLatch(1);
        try (var context = Zlink.createContext();
             var source = meshNode(context);
             var target = meshNode(context)) {
            source.setRoutingId(sourceRid);
            source.setBind(
                "inproc://jvm-infrastructure-source-" + System.nanoTime());
            target.setRoutingId(targetRid);
            target.setBind(endpoint);
            target.startDispatch(record -> {
                applicationEntered.countDown();
                try {
                    releaseApplication.await(2, TimeUnit.SECONDS);
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                } finally {
                    record.close();
                }
            });
            CompletableFuture<byte[]> controlReceived =
                new CompletableFuture<>();
            target.setCanonicalRelocationControlHandler(
                (actualSource, command) -> {
                    assertEquals(sourceRid, actualSource);
                    controlReceived.complete(command);
                    return CompletableFuture.completedFuture(null);
                });
            source.start();
            target.start();
            source.connectPeer(endpoint, targetRid);
            awaitAdmitted(source);

            try (Message packet = Message.from("application.block");
                 Message payload = Message.from(new byte[] {1})) {
                source.spotNode().sendToNode(
                        targetRid,
                        List.of(packet, payload))
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);
            }
            assertTrue(applicationEntered.await(2, TimeUnit.SECONDS));

            source.sendCanonicalRelocationControl(targetRid, command30)
                .toCompletableFuture()
                .get(2, TimeUnit.SECONDS);
            assertArrayEquals(
                command30,
                controlReceived.get(2, TimeUnit.SECONDS));
        } finally {
            releaseApplication.countDown();
        }
    }

    @Test
    void messageFollowIsDeliveredAsInfrastructureWithoutApplicationDispatch()
        throws Exception {
        String endpoint =
            "inproc://jvm-message-follow-target-" + System.nanoTime();
        RoutingId sourceRid = RoutingId.from("jvm-message-follow-source");
        RoutingId targetRid = RoutingId.from("jvm-message-follow-target");
        var route = new ZLinkServiceMessageFollowWireCodec.SpotRoute(
            "spot",
            7,
            targetRid,
            9,
            11,
            13);
        var notice = new ZLinkServiceMessageFollowWireCodec.Notice(
            route,
            route,
            1,
            2,
            1024,
            17,
            19,
            0);
        try (var context = Zlink.createContext();
             var source = meshNode(context);
             var target = meshNode(context)) {
            source.setRoutingId(sourceRid);
            source.setBind(
                "inproc://jvm-message-follow-source-" + System.nanoTime());
            target.setRoutingId(targetRid);
            target.setBind(endpoint);
            AtomicInteger applicationDispatches = new AtomicInteger();
            target.startDispatch(record -> {
                applicationDispatches.incrementAndGet();
                record.close();
            });
            CompletableFuture<ZLinkServiceMessageFollowWireCodec.Notice>
                received = new CompletableFuture<>();
            target.setMessageFollowHandler((actualSource, actualNotice) -> {
                assertEquals(sourceRid, actualSource);
                received.complete(actualNotice);
            });
            source.start();
            target.start();
            source.connectPeer(endpoint, targetRid);
            awaitAdmitted(source);

            source.sendMessageFollow(targetRid, notice)
                .toCompletableFuture()
                .get(2, TimeUnit.SECONDS);

            assertEquals(notice, received.get(2, TimeUnit.SECONDS));
            assertEquals(0, applicationDispatches.get());
        }
    }

    @Test
    void infrastructureControlRejectsApplicationAndUnboundedMultipart() {
        var codec = new ZLinkServiceM6AWireCodec();
        try (Message liveness = Message.from(
                new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceWireCodec()
                    .encode(new systems.zlink.framework.runtime.internal.service
                        .ZLinkServiceWireFrame(
                            systems.zlink.framework.runtime.protocol
                                .ServiceWireConstants.COMMAND_LIVENESS_PROBE,
                            0,
                            List.of(ByteBuffer
                                .allocate(Long.BYTES)
                                .putLong(1)
                                .array()))).getFirst());
             Message application =
                 Message.from(codec.encodeNodeSendHeader(0));
             Message reply = Message.from(
                 new byte[] {90, 77, 1, 20, 0});
             Message relocationData = Message.from(
                 new byte[] {90, 77, 1, 31, 0});
             Message replyRelay = Message.from(
                 new byte[] {90, 77, 1, 33, 0});
             Message relayPayload = Message.from(new byte[] {1})) {
            assertEquals(
                systems.zlink.framework.runtime.protocol
                    .ServiceWireConstants.COMMAND_LIVENESS_PROBE,
                ZLinkJavaRawMeshNode.allowedInfrastructureControlCommand(
                    List.of(liveness.toByteArray())));
            assertEquals(
                -1,
                ZLinkJavaRawMeshNode.allowedInfrastructureControlCommand(
                    List.of(application.toByteArray())));
            assertEquals(
                systems.zlink.framework.runtime.protocol
                    .ServiceWireConstants.COMMAND_REPLY,
                ZLinkJavaRawMeshNode.allowedInfrastructureControlCommand(
                    List.of(reply.toByteArray())));
            assertEquals(
                systems.zlink.framework.runtime.protocol
                    .ServiceWireConstants.COMMAND_RELOCATION_DATA,
                ZLinkJavaRawMeshNode.allowedInfrastructureControlCommand(
                    List.of(relocationData.toByteArray())));
            assertEquals(
                systems.zlink.framework.runtime.protocol
                    .ServiceWireConstants.COMMAND_REPLY_RELAY,
                ZLinkJavaRawMeshNode.allowedInfrastructureControlCommand(
                    List.of(replyRelay.toByteArray())));
            assertEquals(
                systems.zlink.framework.runtime.protocol
                    .ServiceWireConstants.COMMAND_REPLY_RELAY,
                ZLinkJavaRawMeshNode.allowedInfrastructureControlCommand(
                    List.of(
                        replyRelay.toByteArray(),
                        relayPayload.toByteArray())));
        }

        try (Message oversized =
                Message.from(new byte[(256 * 1024) + 1])) {
            assertEquals(
                -1,
                ZLinkJavaRawMeshNode.allowedInfrastructureControlCommand(
                    List.of(oversized.toByteArray())));
        }

        List<Message> tooMany = IntStream.range(0, 65)
            .mapToObj(ignored -> Message.from(new byte[0]))
            .toList();
        try {
            assertEquals(
                -1,
                ZLinkJavaRawMeshNode.allowedInfrastructureControlCommand(
                    tooMany.stream().map(Message::toByteArray).toList()));
        } finally {
            tooMany.forEach(Message::close);
        }
    }

    @Test
    void nodeSendUsesOnlyRawPublicBindingAndDispatchesOwnedParts() throws Exception {
        String endpoint = "inproc://jvm-m6a-" + System.nanoTime();
        RoutingId leftRid = RoutingId.from("jvm-m6a-left");
        RoutingId rightRid = RoutingId.from("jvm-m6a-right");
        try (var context = Zlink.createContext();
             var left = meshNode(context);
             var right = meshNode(context)) {
            context.options().autoHwmEnabled(false);
            left.setRoutingId(leftRid);
            left.setBind(endpoint);
            right.setRoutingId(rightRid);
            right.setBind("inproc://jvm-m6a-right-" + System.nanoTime());
            left.start();
            right.start();
            right.connectPeer(endpoint, leftRid);
            awaitAdmitted(right);

            CompletableFuture<ZLinkMeshDispatchRecord> received =
                new CompletableFuture<>();
            AtomicReference<CompletableFuture<ZLinkMeshDispatchRecord>>
                nextDispatch = new AtomicReference<>(received);
            left.startDispatch(record -> {
                if (!nextDispatch.get().complete(record)) {
                    record.close();
                }
            });
            Message packet = Message.from("packet");
            Message payload = Message.from(new byte[] {1, 2, 3});
            try {
                right.spotNode().sendToNode(
                        leftRid,
                        List.of(packet, payload))
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);
            } finally {
                packet.close();
                payload.close();
            }

            ZLinkMeshDispatchRecord record = received.get(2, TimeUnit.SECONDS);
            try {
                assertEquals(RecordKind.NODE_SEND, record.receive().kind());
                assertEquals(rightRid, record.receive().sourceNodeRid());
                assertEquals("application/json", record.receive().contentType());
                assertEquals("packet", record.parts().getFirst().toUtf8String());
                assertArrayEquals(
                    new byte[] {1, 2, 3},
                    record.parts().get(1).toByteArray());
                // The service envelope has one protocol header and one payload
                // frame. Both native receive claims remain owned by this record
                // until its terminal close.
                awaitOutstandingApplicationLease(context, 2L);
            } finally {
                record.close();
            }
            awaitOutstandingApplicationLease(context, 0L);
            CompletableFuture<ZLinkMeshDispatchRecord> progressed =
                new CompletableFuture<>();
            nextDispatch.set(progressed);
            sendNodePayload(right, leftRid, new byte[] {4});
            try (ZLinkMeshDispatchRecord next =
                progressed.get(2, TimeUnit.SECONDS)) {
                assertEquals(RecordKind.NODE_SEND, next.receive().kind());
            }
    }
    }

    private static void closeCompletedRecord(
        CompletableFuture<ZLinkMeshDispatchRecord> future) {
        if (!future.isDone() || future.isCompletedExceptionally()) {
            return;
        }
        ZLinkMeshDispatchRecord record = future.getNow(null);
        if (record != null) {
            record.close();
        }
    }

    private static void sendNodePayload(
        ZLinkJavaRawMeshNode source,
        RoutingId targetRid,
        byte[] payloadBytes) throws Exception {
        try (Message packet = Message.from("hwm.packet");
             Message payload = Message.from(payloadBytes)) {
            source.spotNode().sendToNode(
                    targetRid,
                    List.of(packet, payload))
                .toCompletableFuture()
                .get(2, TimeUnit.SECONDS);
        }
    }

    @Test
    void nodeRequestCompletesExactlyOnceThroughFrameworkReply() throws Exception {
        String endpoint = "inproc://jvm-m6a-request-" + System.nanoTime();
        RoutingId leftRid = RoutingId.from("jvm-m6a-request-left");
        RoutingId rightRid = RoutingId.from("jvm-m6a-request-right");
        try (var context = Zlink.createContext();
             var left = meshNode(context);
             var right = meshNode(context)) {
            left.setRoutingId(leftRid);
            left.setBind(endpoint);
            right.setRoutingId(rightRid);
            right.setBind("inproc://jvm-m6a-request-right-" + System.nanoTime());
            left.start();
            right.start();
            right.connectPeer(endpoint, leftRid);
            awaitAdmitted(right);
            left.startDispatch(record -> {
                try (record;
                     Message packet = Message.from("reply");
                     Message payload = Message.from(new byte[] {9, 8, 7})) {
                    assertEquals(RecordKind.NODE_REQUEST, record.receive().kind());
                    assertNotNull(record.receive().applicationCorrelation());
                    record.reply(List.of(packet, payload));
                }
            });

            CompletableFuture<ZLinkBackendReceived> completed =
                new CompletableFuture<>();
            try (Message packet = Message.from("request");
                 Message payload = Message.from(new byte[] {1})) {
                completed = right.spotNode().requestToNode(
                        leftRid,
                        List.of(packet, payload),
                        Duration.ofSeconds(2))
                    .toCompletableFuture();
            }

            try (ZLinkBackendReceived reply =
                completed.get(2, TimeUnit.SECONDS)) {
                assertEquals(ZLinkBackendRequestResult.OK, reply.result());
                assertEquals(2, reply.parts().size());
                assertEquals("reply", reply.parts().getFirst().toUtf8String());
                assertArrayEquals(
                    new byte[] {9, 8, 7},
                    reply.parts().get(1).toByteArray());
            }
        }
    }

    @Test
    void boundActorRequestDecodesOneFrameTerminalError() throws Exception {
        String endpoint = "inproc://jvm-m6a-bound-error-"
            + System.nanoTime();
        RoutingId sourceRid = RoutingId.from("jvm-m6a-bound-error-source");
        RoutingId targetRid = RoutingId.from("jvm-m6a-bound-error-target");
        try (var context = Zlink.createContext();
             var source = meshNode(context);
             var target = meshNode(context)) {
            source.setRoutingId(sourceRid);
            source.setBind(
                "inproc://jvm-m6a-bound-error-source-" + System.nanoTime());
            target.setRoutingId(targetRid);
            target.setBind(endpoint);
            target.setPeerAuthorityResolver(
                (meshName, candidateRid, candidateGeneration) ->
                    CompletableFuture.completedFuture(
                        Optional.of(new ZLinkInternalMeshNode.PeerAuthorityFence(
                            candidateRid,
                            candidateGeneration,
                            "test-owner",
                            1))));
            source.start();
            target.start();
            source.connectPeer(endpoint, targetRid);
            awaitAdmitted(source);

            var actor = new ZLinkBackendActorRef(
                targetRid, "missing-bound-actor", 1);
            source.spotNode().rememberActorAuthority(actor, 1, 1);
            CompletionStage<List<Message>> request;
            try (Message payload = Message.from("bound-request")) {
                request = source.requestBoundActorAsync(
                    actor,
                    RoutingId.from("bound-session"),
                    1,
                    1,
                    1,
                    List.of(payload),
                    Duration.ofSeconds(2));
            }

            var failure = assertThrows(
                ExecutionException.class,
                () -> request.toCompletableFuture().get(2, TimeUnit.SECONDS));
            //  The bound-Actor reply decode now classifies the carried
            //  terminal via the ownership-aware translator (spec
            //  32-framework-error-model:83-118) instead of surfacing the raw
            //  ZlinkRequestException; the original transport exception is
            //  preserved as the cause for terminal-shape probes.
            assertTrue(
                failure.getCause()
                    instanceof systems.zlink.framework.errors.ZLinkFrameworkException);
            assertEquals(
                systems.zlink.framework.errors.ZLinkFrameworkErrorKind.NOT_FOUND,
                ((systems.zlink.framework.errors.ZLinkFrameworkException)
                        failure.getCause())
                    .kind());
            assertTrue(
                failure.getCause().getCause() instanceof ZlinkRequestException);
            assertEquals(
                RequestResult.NOT_FOUND,
                ((ZlinkRequestException) failure.getCause().getCause())
                    .getResult());
        }
    }

    // Production wires the host Application Job Queue onto every mesh node
    // (ZLinkMeshNodeRuntime.start). These tests drive the raw node directly,
    // so application dispatch (drainApplicationMailbox) silently no-ops until
    // the queue is set. Build a node with the queue attached, mirroring the
    // production wiring.
    private static ZLinkJavaRawMeshNode meshNode(Context context) {
        ZLinkJavaRawMeshNode node = new ZLinkJavaRawMeshNode(context, "mesh");
        node.setApplicationJobQueue(new ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueProfile.BALANCED,
            OptionalLong.empty(),
            new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null)));
        return node;
    }

    private static void awaitAdmitted(ZLinkJavaRawMeshNode node)
        throws InterruptedException {
        long deadline =
            System.nanoTime() + Duration.ofSeconds(2).toNanos();
        while (node.peers().stream().noneMatch(peer ->
                peer.state()
                    == systems.zlink.framework.runtime.internal.binding.spot
                        .MeshPeerState.ADMITTED)
            && System.nanoTime() < deadline) {
            Thread.sleep(1);
        }
        assertTrue(node.peers().stream().anyMatch(peer ->
            peer.state()
                == systems.zlink.framework.runtime.internal.binding.spot
                    .MeshPeerState.ADMITTED));
    }

    private static byte[] canonicalRelocationCommand(int command)
        throws Exception {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path fixture = current.resolve(
                "runtime/protocol/golden/relocation-control-v1.json");
            if (Files.isRegularFile(fixture)) {
                var canonical = new ObjectMapper()
                    .readTree(Files.readString(fixture))
                    .path("canonical");
                for (var entry : canonical) {
                    if (entry.path("command").asInt() == command) {
                        return HexFormat.of().parseHex(
                            entry.path("hex").asText());
                    }
                }
                throw new IllegalStateException(
                    "canonical relocation command is unavailable: " + command);
            }
            current = current.getParent();
        }
        throw new IllegalStateException(
            "shared relocation fixture was not found");
    }

    private static void awaitState(
        ZLinkJavaRawMeshNode node,
        MeshPeerState state) throws InterruptedException {
        long deadline =
            System.nanoTime() + Duration.ofSeconds(2).toNanos();
        while (node.peers().stream().noneMatch(
                peer -> peer.state() == state)
            && System.nanoTime() < deadline) {
            Thread.sleep(1);
        }
        assertTrue(node.peers().stream().anyMatch(
            peer -> peer.state() == state));
    }

    private static long awaitReplacement(
        ZLinkJavaRawMeshNode node,
        String endpoint,
        RoutingId expectedRoutingId,
        long expectedLifecycleGeneration,
        String expectedSecurityIdentity)
        throws InterruptedException {
        long deadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
        RuntimeException last = null;
        while (System.nanoTime() < deadline) {
            try {
                return node.replacePeerConnection(
                    endpoint,
                    expectedRoutingId,
                    expectedLifecycleGeneration,
                    expectedSecurityIdentity);
            } catch (RuntimeException retryableFailure) {
                last = retryableFailure;
                Thread.onSpinWait();
            }
        }
        if (last != null) {
            throw last;
        }
        throw new AssertionError("peer replacement did not run");
    }

    private static void awaitTransport(
        ZLinkJavaRawMeshNode node,
        String endpoint) throws InterruptedException {
        long deadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
        while (!node.hasLivePeerIntent(endpoint)
            && System.nanoTime() < deadline) {
            Thread.onSpinWait();
        }
        assertTrue(node.hasLivePeerIntent(endpoint));
    }

    private static void awaitOutstandingApplicationLease(
        systems.zlink.contracts.core.Context context,
        long expected) throws InterruptedException {
        long deadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
        while (context.coreHwmBudgetSnapshot()
            .outstandingApplicationLeaseCount() != expected
            && System.nanoTime() < deadline) {
            Thread.sleep(1);
        }
        assertEquals(expected, context.coreHwmBudgetSnapshot()
            .outstandingApplicationLeaseCount());
    }
}
