package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.ObjectMapper;
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
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
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
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

final class ZLinkJavaRawMeshNodeM6ATest {
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
            1024,
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
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
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
             var left = new ZLinkJavaRawMeshNode(context, "mesh");
             var right = new ZLinkJavaRawMeshNode(context, "mesh")) {
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
             var left = new ZLinkJavaRawMeshNode(context, "mesh");
             var right = new ZLinkJavaRawMeshNode(context, "mesh")) {
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
             var local = new ZLinkJavaRawMeshNode(context, "mesh")) {
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
             var lower = new ZLinkJavaRawMeshNode(context, "mesh");
             var higher = new ZLinkJavaRawMeshNode(context, "mesh")) {
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
                long deadline =
                    System.nanoTime() + Duration.ofSeconds(2).toNanos();
                boolean submitted = false;
                while (!submitted && System.nanoTime() < deadline) {
                    submitted = lower.spotNode().sendToNode(
                        higherRid,
                        List.of(packet, payload),
                        SendFlags.DONT_WAIT);
                    if (!submitted) {
                        Thread.sleep(1);
                    }
                }
                assertTrue(submitted);
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
    void command44ReceivesCommand45ThroughInfrastructureDispatcher()
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
            10, 11, sourceRid, 12, 17);
        try (var context = Zlink.createContext();
             var source = new ZLinkJavaRawMeshNode(context, "mesh");
             var sessionOwner = new ZLinkJavaRawMeshNode(context, "mesh")) {
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
                return CompletableFuture.completedFuture(
                    codec.encodeSessionRelocationRouted(
                        new ZLinkServiceM6BWireCodec.SessionRelocationRouted(
                            command.relocation(), command.coordinator(),
                            command.actor(), command.session(), command.action(),
                            command.currentAuthorityOwnerGeneration(),
                            command.lastAcceptedSessionSequence())));
            });
            source.start();
            sessionOwner.start();
            source.connectPeer(endpoint, sessionOwnerRid);
            awaitAdmitted(source);

            var ack = codec.decodeSessionRelocationRouted(
                source.requestSessionRelocationRoute(
                        sessionOwnerRid,
                        codec.encodeSessionRelocationRoute(command),
                        Duration.ofSeconds(2))
                    .toCompletableFuture().get(2, TimeUnit.SECONDS));

            assertEquals(command.relocation(), ack.relocation());
            assertEquals(17, ack.lastAcceptedSessionSequence());
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
             var source = new ZLinkJavaRawMeshNode(context, "mesh");
             var target = new ZLinkJavaRawMeshNode(context, "mesh")) {
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
             var source = new ZLinkJavaRawMeshNode(context, "mesh");
             var target = new ZLinkJavaRawMeshNode(context, "mesh")) {
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
    void completionControlProgressesWhileApplicationDispatchIsBlocked()
        throws Exception {
        String endpoint =
            "inproc://jvm-completion-control-" + System.nanoTime();
        RoutingId sourceRid = RoutingId.from("jvm-completion-source");
        RoutingId targetRid = RoutingId.from("jvm-completion-target");
        byte[] command30 = canonicalRelocationCommand(30);
        CountDownLatch applicationEntered = new CountDownLatch(1);
        CountDownLatch releaseApplication = new CountDownLatch(1);
        try (var context = Zlink.createContext();
             var source = new ZLinkJavaRawMeshNode(context, "mesh");
             var target = new ZLinkJavaRawMeshNode(context, "mesh")) {
            source.setRoutingId(sourceRid);
            source.setBind(
                "inproc://jvm-completion-source-" + System.nanoTime());
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
                long deadline =
                    System.nanoTime() + Duration.ofSeconds(2).toNanos();
                boolean submitted = false;
                while (!submitted && System.nanoTime() < deadline) {
                    submitted = source.spotNode().sendToNode(
                        targetRid,
                        List.of(packet, payload),
                        SendFlags.DONT_WAIT);
                    if (!submitted) {
                        Thread.sleep(1);
                    }
                }
                assertTrue(submitted);
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
             var source = new ZLinkJavaRawMeshNode(context, "mesh");
             var target = new ZLinkJavaRawMeshNode(context, "mesh")) {
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
    void completionControlRejectsApplicationAndUnboundedMultipart() {
        var codec = new ZLinkServiceM6AWireCodec();
        try (Message liveness = Message.from(
                new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceWireCodec()
                    .encode(new systems.zlink.framework.runtime.internal.service
                        .ZLinkServiceWireFrame(
                            systems.zlink.framework.runtime.protocol
                                .ServiceWireConstants.COMMAND_LIVENESS_PROBE,
                            0,
                            List.of(java.nio.ByteBuffer
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
                ZLinkJavaRawMeshNode.allowedCompletionControlCommand(
                    List.of(liveness)));
            assertEquals(
                -1,
                ZLinkJavaRawMeshNode.allowedCompletionControlCommand(
                    List.of(application)));
            assertEquals(
                -1,
                ZLinkJavaRawMeshNode.allowedCompletionControlCommand(
                    List.of(reply)));
            assertEquals(
                -1,
                ZLinkJavaRawMeshNode.allowedCompletionControlCommand(
                    List.of(relocationData)));
            assertEquals(
                systems.zlink.framework.runtime.protocol
                    .ServiceWireConstants.COMMAND_REPLY_RELAY,
                ZLinkJavaRawMeshNode.allowedCompletionControlCommand(
                    List.of(replyRelay)));
            assertEquals(
                -1,
                ZLinkJavaRawMeshNode.allowedCompletionControlCommand(
                    List.of(replyRelay, relayPayload)));
        }

        try (Message oversized =
                Message.from(new byte[(256 * 1024) + 1])) {
            assertEquals(
                -1,
                ZLinkJavaRawMeshNode.allowedCompletionControlCommand(
                    List.of(oversized)));
        }

        List<Message> tooMany = java.util.stream.IntStream.range(0, 65)
            .mapToObj(ignored -> Message.from(new byte[0]))
            .toList();
        try {
            assertEquals(
                -1,
                ZLinkJavaRawMeshNode.allowedCompletionControlCommand(
                    tooMany));
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
             var left = new ZLinkJavaRawMeshNode(context, "mesh");
             var right = new ZLinkJavaRawMeshNode(context, "mesh")) {
            left.setRoutingId(leftRid);
            left.setBind(endpoint);
            right.setRoutingId(rightRid);
            right.setBind("inproc://jvm-m6a-right-" + System.nanoTime());
            left.start();
            right.start();
            right.connectPeer(endpoint, leftRid);

            CompletableFuture<ZLinkMeshDispatchRecord> received =
                new CompletableFuture<>();
            left.startDispatch(received::complete);
            Message packet = Message.from("packet");
            Message payload = Message.from(new byte[] {1, 2, 3});
            try {
                long deadline =
                    System.nanoTime() + Duration.ofSeconds(2).toNanos();
                boolean submitted = false;
                while (!submitted && System.nanoTime() < deadline) {
                    submitted = right.spotNode().sendToNode(
                        leftRid,
                        List.of(packet, payload),
                        SendFlags.DONT_WAIT);
                    if (!submitted) {
                        Thread.sleep(1);
                    }
                }
                assertTrue(submitted);
            } finally {
                packet.close();
                payload.close();
            }

            try (ZLinkMeshDispatchRecord record =
                received.get(2, TimeUnit.SECONDS)) {
                assertEquals(RecordKind.NODE_SEND, record.receive().kind());
                assertEquals(rightRid, record.receive().sourceNodeRid());
                assertEquals("application/json", record.receive().contentType());
                assertEquals("packet", record.parts().getFirst().toUtf8String());
                assertArrayEquals(
                    new byte[] {1, 2, 3},
                    record.parts().get(1).toByteArray());
            }
        }
    }

    @Test
    void applicationHwmPausesRawReceiveUntilDispatchRecordCloses()
        throws Exception {
        String endpoint = "inproc://jvm-m6a-hwm-" + System.nanoTime();
        RoutingId targetRid = RoutingId.from("jvm-m6a-hwm-target");
        RoutingId sourceRid = RoutingId.from("jvm-m6a-hwm-source");
        ZLinkInboundDispatchBudget budget =
            new ZLinkInboundDispatchBudget(1);
        CompletableFuture<ZLinkMeshDispatchRecord> first =
            new CompletableFuture<>();
        CompletableFuture<ZLinkMeshDispatchRecord> second =
            new CompletableFuture<>();
        try (var context = Zlink.createContext();
             var target = new ZLinkJavaRawMeshNode(context, "mesh");
             var source = new ZLinkJavaRawMeshNode(context, "mesh")) {
            target.setRoutingId(targetRid);
            target.setBind(endpoint);
            target.setApplicationDispatchBudget(budget);
            target.startDispatch(record -> {
                if (!first.complete(record)) {
                    second.complete(record);
                }
            });
            source.setRoutingId(sourceRid);
            source.setBind(
                "inproc://jvm-m6a-hwm-source-" + System.nanoTime());
            target.start();
            source.start();
            source.connectPeer(endpoint, targetRid);
            awaitAdmitted(source);

            sendNodePayload(source, targetRid, new byte[] {1});
            ZLinkMeshDispatchRecord firstRecord =
                first.get(2, TimeUnit.SECONDS);
            assertEquals(
                new ZLinkInboundDispatchBudget.Snapshot(1, 1, 1, 0, true),
                budget.snapshot());

            sendNodePayload(source, targetRid, new byte[] {2});
            assertThrows(
                TimeoutException.class,
                () -> second.get(100, TimeUnit.MILLISECONDS));
            assertEquals(1, budget.snapshot().pendingPayloadBytes());

            firstRecord.close();
            try (ZLinkMeshDispatchRecord secondRecord =
                second.get(2, TimeUnit.SECONDS)) {
                assertEquals(RecordKind.NODE_SEND, secondRecord.receive().kind());
                assertEquals(1, budget.snapshot().pendingPayloadBytes());
            }
            assertEquals(0, budget.snapshot().pendingPayloadBytes());
        } finally {
            closeCompletedRecord(first);
            closeCompletedRecord(second);
        }
    }

    @Test
    void closingRawSpotReleasesQueuedSubscriptionAdmissionLease() {
        ZLinkInboundDispatchBudget budget =
            new ZLinkInboundDispatchBudget(8);
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            ZLinkJavaRawSpotNode spotNode = new ZLinkJavaRawSpotNode(node);
            ZLinkJavaRawSpot spot = new ZLinkJavaRawSpot(spotNode, "spot", 1);
            List<Message> parts = List.of(Message.from("payload"));
            ZLinkInboundDispatchBudget.Lease lease = budget.track(7);

            assertTrue(spot.enqueueTopic(new ZLinkBackendTopicMessage(
                Optional.empty(),
                "channel",
                "topic",
                new byte[0],
                parts,
                "application/json",
                lease)));

            spot.close();

            assertEquals(0, budget.snapshot().pendingPayloadBytes());
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
        byte[] payloadBytes) throws InterruptedException {
        try (Message packet = Message.from("hwm.packet");
             Message payload = Message.from(payloadBytes)) {
            long deadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
            boolean submitted = false;
            while (!submitted && System.nanoTime() < deadline) {
                submitted = source.spotNode().sendToNode(
                    targetRid,
                    List.of(packet, payload),
                    SendFlags.DONT_WAIT);
                if (!submitted) {
                    Thread.sleep(1);
                }
            }
            assertTrue(submitted);
        }
    }

    @Test
    void nodeRequestCompletesExactlyOnceThroughFrameworkReply() throws Exception {
        String endpoint = "inproc://jvm-m6a-request-" + System.nanoTime();
        RoutingId leftRid = RoutingId.from("jvm-m6a-request-left");
        RoutingId rightRid = RoutingId.from("jvm-m6a-request-right");
        try (var context = Zlink.createContext();
             var left = new ZLinkJavaRawMeshNode(context, "mesh");
             var right = new ZLinkJavaRawMeshNode(context, "mesh")) {
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
                long deadline =
                    System.nanoTime() + Duration.ofSeconds(2).toNanos();
                boolean submitted = false;
                while (!submitted && System.nanoTime() < deadline) {
                    submitted = right.spotNode().requestToNode(
                        leftRid,
                        List.of(packet, payload),
                        completed::complete,
                        SendFlags.DONT_WAIT,
                        Duration.ofSeconds(2));
                    if (!submitted) {
                        Thread.sleep(1);
                    }
                }
                assertTrue(submitted);
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
             var source = new ZLinkJavaRawMeshNode(context, "mesh");
             var target = new ZLinkJavaRawMeshNode(context, "mesh")) {
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
                java.util.concurrent.ExecutionException.class,
                () -> request.toCompletableFuture().get(2, TimeUnit.SECONDS));
            assertTrue(failure.getCause() instanceof ZlinkRequestException);
            assertEquals(
                RequestResult.NOT_FOUND,
                ((ZlinkRequestException) failure.getCause()).getResult());
        }
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
}
