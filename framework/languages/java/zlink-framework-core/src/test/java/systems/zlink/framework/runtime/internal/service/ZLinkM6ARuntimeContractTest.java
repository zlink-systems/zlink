package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;

final class ZLinkM6ARuntimeContractTest {
    @Test
    void topologyFencesStaleConnectionsAndSelectsOnlyServingPositiveWeight() {
        var topology = new ZLinkServiceTopologyRegistry(
            descriptor(
                "mesh",
                "local",
                1,
                1,
                List.of(new ZLinkServiceNodeDescriptor.Channel("orders", 100)),
                100));
        var first = descriptor(
            "mesh",
            "peer-a",
            9,
            2,
            List.of(new ZLinkServiceNodeDescriptor.Channel("orders", 0)),
            100);
        var updated = descriptor(
            "mesh",
            "peer-a",
            9,
            3,
            List.of(new ZLinkServiceNodeDescriptor.Channel("orders", 7)),
            100);

        assertEquals(
            ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
            topology.admit(first, "pipe-old"));
        assertTrue(topology.selectChannel("orders").isEmpty());
        assertFalse(topology.hasSelectableChannel("orders"));
        assertEquals(
            ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
            topology.admit(updated, "pipe-current"));
        assertEquals(
            RoutingId.from("peer-a"),
            topology.selectChannel("orders")
                .orElseThrow()
                .descriptor()
                .nodeRoutingId());
        assertFalse(topology.disconnect(RoutingId.from("peer-a"), "pipe-old"));
        assertTrue(topology.peer(RoutingId.from("peer-a")).isPresent());
        assertTrue(topology.disconnect(
            RoutingId.from("peer-a"), "pipe-current"));
    }

    @Test
    void initialAdmissionRequiresExactEndpointSecurityAndLifecycle() {
        var peer = descriptor(
            "mesh",
            "peer",
            7,
            1,
            List.of(),
            100);

        assertTrue(ZLinkServiceAdmissionGuard.matchesExpectedRoute(
            "inproc://peer", "default", 7, peer));
        assertFalse(ZLinkServiceAdmissionGuard.matchesExpectedRoute(
            "inproc://other", "default", 7, peer));
        assertFalse(ZLinkServiceAdmissionGuard.matchesExpectedRoute(
            "inproc://peer", "other-identity", 7, peer));
        assertFalse(ZLinkServiceAdmissionGuard.matchesExpectedRoute(
            "inproc://peer", "default", 8, peer));
    }

    @Test
    void higherRevisionRejectsEveryImmutableDescriptorMutation() {
        var peer = descriptor(
            "mesh",
            "peer",
            7,
            1,
            List.of(new ZLinkServiceNodeDescriptor.Channel("orders", 100)),
            100);

        for (String field : List.of(
                "endpoint",
                "securityIdentity",
                "channelSet",
                "objectRole",
                "effectiveMaxMessageBytes",
                "applicationVersion",
                "protocolCapabilities",
                "activeCapacityLimit",
                "pendingCapacityLimit")) {
            var topology = new ZLinkServiceTopologyRegistry(
                descriptor("mesh", "local", 1, 1, List.of(), 100));
            assertEquals(
                ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
                topology.admit(peer, "pipe"));
            assertEquals(
                ZLinkServiceTopologyRegistry.AdmissionResult.INVALID_DESCRIPTOR,
                topology.admit(
                    immutableMutation(peer, field),
                    "pipe"),
                field);
            assertEquals(
                peer,
                topology.peer(RoutingId.from("peer"))
                    .orElseThrow()
                    .descriptor(),
                field);
        }
    }

    @Test
    void duplicateAdmissionKeepsCanonicalDirectionAndLateCloseCannotRemoveIt() {
        RoutingId lower = RoutingId.from("a");
        RoutingId higher = RoutingId.from("z");
        var topology = new ZLinkServiceTopologyRegistry(
            descriptor("mesh", "a", 1, 1, List.of(), 100));
        var peer = descriptor(
            "mesh",
            "z",
            9,
            1,
            List.of(new ZLinkServiceNodeDescriptor.Channel("orders", 1)),
            100);

        assertEquals(
            ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
            topology.admit(
                peer,
                connection(
                    "outbound-current",
                    ZLinkServiceAdmissionGuard.ConnectionDirection.OUTBOUND)));
        assertEquals(
            ZLinkServiceTopologyRegistry.AdmissionResult.DUPLICATE_REJECTED,
            topology.admit(
                peer,
                connection(
                    "inbound-duplicate",
                    ZLinkServiceAdmissionGuard.ConnectionDirection.INBOUND)));
        assertEquals(
            "outbound-current",
            topology.peer(higher).orElseThrow().connectionId());

        assertFalse(topology.disconnect(higher, "inbound-duplicate"));
        assertEquals(
            "outbound-current",
            topology.peer(higher).orElseThrow().connectionId());

        assertEquals(
            ZLinkServiceAdmissionGuard.DuplicateConnectionDecision.KEEP_CURRENT,
            ZLinkServiceAdmissionGuard.selectConnection(
                lower,
                higher,
                9,
                ZLinkServiceAdmissionGuard.ConnectionDirection.OUTBOUND,
                "outbound-current",
                9,
                ZLinkServiceAdmissionGuard.ConnectionDirection.INBOUND,
                "inbound-duplicate"));
        assertEquals(
            ZLinkServiceAdmissionGuard.DuplicateConnectionDecision.KEEP_CURRENT,
            ZLinkServiceAdmissionGuard.selectConnection(
                higher,
                lower,
                9,
                ZLinkServiceAdmissionGuard.ConnectionDirection.INBOUND,
                "inbound-current",
                9,
                ZLinkServiceAdmissionGuard.ConnectionDirection.OUTBOUND,
                "outbound-duplicate"));

        var replacement = new ZLinkServiceTopologyRegistry(
            descriptor("mesh", "a", 1, 1, List.of(), 100));
        assertEquals(
            ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
            replacement.admit(
                peer,
                connection(
                    "inbound-old",
                    ZLinkServiceAdmissionGuard.ConnectionDirection.INBOUND)));
        assertEquals(
            ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
            replacement.admit(
                peer,
                connection(
                    "outbound-replacement",
                    ZLinkServiceAdmissionGuard.ConnectionDirection.OUTBOUND)));
        assertFalse(replacement.disconnect(higher, "inbound-old"));
        assertEquals(
            "outbound-replacement",
            replacement.peer(higher).orElseThrow().connectionId());
    }

    @Test
    void commonWeightsUseExactRangeRatioRevisionAndCapacityEligibility() {
        assertEquals(
            10_000,
            new ZLinkServiceNodeDescriptor.Channel(
                "maximum",
                10_000).weight());
        org.junit.jupiter.api.Assertions.assertThrows(
            IllegalArgumentException.class,
            () -> new ZLinkServiceNodeDescriptor.Channel("negative", -1));
        org.junit.jupiter.api.Assertions.assertThrows(
            IllegalArgumentException.class,
            () -> new ZLinkServiceNodeDescriptor.Channel(
                "too-large",
                10_001));

        var topology = new ZLinkServiceTopologyRegistry(
            descriptor("mesh", "local", 1, 1, List.of(), 100));
        topology.admit(
            descriptor(
                "mesh",
                "peer-a",
                1,
                1,
                List.of(new ZLinkServiceNodeDescriptor.Channel(
                    "orders",
                    100)),
                100),
            "pipe-a");
        topology.admit(
            descriptor(
                "mesh",
                "peer-b",
                1,
                1,
                List.of(new ZLinkServiceNodeDescriptor.Channel(
                    "orders",
                    300)),
                300),
            "pipe-b");
        int selectedA = 0;
        int selectedB = 0;
        for (int index = 0; index < 400; index++) {
            String rid = topology.selectChannel("orders")
                .orElseThrow()
                .descriptor()
                .nodeRoutingId()
                .toString();
            if ("peer-a".equals(rid)) {
                selectedA++;
            } else if ("peer-b".equals(rid)) {
                selectedB++;
            }
        }
        assertEquals(100, selectedA);
        assertEquals(300, selectedB);

        assertEquals(
            ZLinkServiceTopologyRegistry.AdmissionResult.STALE_DESCRIPTOR,
            topology.admit(
                descriptor(
                    "mesh",
                    "peer-b",
                    1,
                    1,
                    List.of(new ZLinkServiceNodeDescriptor.Channel(
                        "orders",
                        0)),
                    0),
                "stale-pipe"));
        assertEquals(
            ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
            topology.admit(
                descriptor(
                    "mesh",
                    "peer-b",
                    1,
                    2,
                    List.of(new ZLinkServiceNodeDescriptor.Channel(
                        "orders",
                        0)),
                    0),
                "current-pipe"));
        for (int index = 0; index < 16; index++) {
            assertEquals(
                RoutingId.from("peer-a"),
                topology.selectChannel("orders")
                    .orElseThrow()
                    .descriptor()
                    .nodeRoutingId());
        }

        var capacityTopology = new ZLinkServiceTopologyRegistry(
            descriptor("mesh", "local", 1, 1, List.of(), 100));
        capacityTopology.admit(
            descriptorWithCapacity(
                "full", 10_000, 100, 10, 100, 10),
            "full-pipe");
        capacityTopology.admit(
            descriptorWithCapacity(
                "eligible", 1, 100, 0, 100, 0),
            "eligible-pipe");
        assertEquals(
            RoutingId.from("eligible"),
            capacityTopology.selectPlacement()
                .orElseThrow()
                .descriptor()
                .nodeRoutingId());
    }

    @Test
    void livenessResendsOneProbeAndOnlyMatchingAckRenewsDeadline() {
        var liveness = new ZLinkServiceLivenessRegistry(
            Duration.ofSeconds(5), Duration.ofSeconds(15));
        RoutingId peer = RoutingId.from("peer");
        liveness.admit(peer, "pipe", 0);

        var first = liveness.tick(Duration.ofSeconds(5).toNanos());
        assertEquals(1, first.probes().size());
        long probe = first.probes().getFirst().probeId();
        var retry = liveness.tick(Duration.ofSeconds(10).toNanos());
        assertEquals(probe, retry.probes().getFirst().probeId());
        assertFalse(liveness.acknowledge(
            peer, "stale-pipe", probe, Duration.ofSeconds(11).toNanos()));
        assertFalse(liveness.acknowledge(
            peer, "pipe", probe + 1, Duration.ofSeconds(11).toNanos()));
        assertTrue(liveness.acknowledge(
            peer, "pipe", probe, Duration.ofSeconds(11).toNanos()));

        assertTrue(liveness.tick(Duration.ofSeconds(20).toNanos())
            .timedOutNodes().isEmpty());
        assertEquals(
            List.of(peer),
            liveness.tick(Duration.ofSeconds(26).toNanos()).timedOutNodes());
    }

    @Test
    void livenessRequiresAProbeAckBeforeAConnectionCanBeSelected() {
        var liveness = new ZLinkServiceLivenessRegistry(
            Duration.ofSeconds(5), Duration.ofSeconds(15));
        RoutingId peer = RoutingId.from("peer-ready");
        long now = 100;

        liveness.admit(peer, "pipe", now);
        assertFalse(liveness.isReady(peer, "pipe"));
        assertTrue(liveness.requestProbe(peer, "pipe", now));
        assertFalse(liveness.requestProbe(peer, "pipe", now));

        var probe = liveness.tick(now).probes().getFirst();
        assertTrue(liveness.acknowledgeProbe(
            peer, "pipe", probe.probeId()).isPresent());
        assertFalse(liveness.isReady(peer, "pipe"));
        assertTrue(liveness.acknowledge(
            peer, "pipe", probe.probeId(), now + 1));
        assertTrue(liveness.isReady(peer, "pipe"));
        assertFalse(liveness.requestProbe(peer, "pipe", now + 1));
    }

    @Test
    void oldConnectionAckCannotReadyOrRenewAReplacementConnection() {
        var liveness = new ZLinkServiceLivenessRegistry(
            Duration.ofSeconds(5), Duration.ofSeconds(15));
        RoutingId peer = RoutingId.from("peer-replaced");

        liveness.admit(peer, "old-pipe", 0);
        long oldProbe = liveness.tick(Duration.ofSeconds(5).toNanos())
            .probes()
            .getFirst()
            .probeId();

        liveness.admit(peer, "new-pipe", Duration.ofSeconds(6).toNanos());
        assertFalse(liveness.acknowledge(
            peer,
            "old-pipe",
            oldProbe,
            Duration.ofSeconds(7).toNanos()));
        assertFalse(liveness.isReady(peer, "new-pipe"));
        assertEquals(
            List.of(peer),
            liveness.tick(Duration.ofSeconds(22).toNanos()).timedOutNodes());
    }

    @Test
    void topologySelectionCanExcludeAnAdmittedButNotReadyPeer() {
        var topology = new ZLinkServiceTopologyRegistry(
            descriptor("mesh", "local", 1, 1, List.of(), 100));
        var peerA = descriptor(
            "mesh", "peer-a", 1, 1,
            List.of(new ZLinkServiceNodeDescriptor.Channel("orders", 100)),
            100);
        var peerB = descriptor(
            "mesh", "peer-b", 1, 1,
            List.of(new ZLinkServiceNodeDescriptor.Channel("orders", 100)),
            100);
        topology.admit(peerA, "pipe-a");
        topology.admit(peerB, "pipe-b");

        assertEquals(
            RoutingId.from("peer-b"),
            topology.selectChannel(
                    "orders",
                    peer -> peer.descriptor().nodeRoutingId()
                        .equals(RoutingId.from("peer-b")))
                .orElseThrow()
                .descriptor()
                .nodeRoutingId());
        assertFalse(topology.hasSelectableChannel(
            "orders", ignored -> false));
    }

    @Test
    void mailboxSerializesEachOwnerAndKeepsInfrastructureReserve() {
        var mailbox = new ZLinkServiceMailbox(2, 8, 1, 8);
        assertTrue(mailbox.tryEnqueue(record(
            "node:a", ZLinkServiceMailbox.Domain.APPLICATION, 4)));
        assertTrue(mailbox.tryEnqueue(record(
            "node:a", ZLinkServiceMailbox.Domain.APPLICATION, 4)));
        assertFalse(mailbox.tryEnqueue(record(
            "node:b", ZLinkServiceMailbox.Domain.APPLICATION, 1)));
        assertTrue(mailbox.tryEnqueue(record(
            "peer:a", ZLinkServiceMailbox.Domain.INFRASTRUCTURE, 8)));

        var first = mailbox.tryClaim(
            ZLinkServiceMailbox.Domain.APPLICATION, 1, 4).orElseThrow();
        assertTrue(mailbox.tryClaim(
            ZLinkServiceMailbox.Domain.APPLICATION, 1, 4).isEmpty());
        assertTrue(mailbox.release(first));
        var second = mailbox.tryClaim(
            ZLinkServiceMailbox.Domain.APPLICATION, 1, 4).orElseThrow();
        assertEquals(first.owner(), second.owner());
        assertNotEquals(first.serial(), second.serial());
        assertTrue(mailbox.tryClaim(
            ZLinkServiceMailbox.Domain.INFRASTRUCTURE, 1, 8).isPresent());
    }

    @Test
    void locationAuthorityPublishesOpaqueCasChangesAndFencesOldVersion() {
        Instant storeNow = Instant.parse("2026-07-23T00:00:00Z");
        var authority = new ZLinkInMemoryLocationAuthority(() -> storeNow);
        List<ZLinkInMemoryLocationAuthority.Change> changes = new ArrayList<>();
        authority.subscribe(changes::add);

        var created = authority.compareExchange(
            "zla1:a:1:a",
            ZLinkInMemoryLocationAuthority.Expectation.expectMissing(),
            ZLinkInMemoryLocationAuthority.Mutation.newObject(
                new byte[] {1, 2, 3}));
        assertEquals(
            ZLinkInMemoryLocationAuthority.CasKind.STORED,
            created.kind());
        byte[] callerCopy = created.snapshot().payload();
        callerCopy[0] = 9;
        assertArrayEquals(
            new byte[] {1, 2, 3},
            authority.read("zla1:a:1:a").snapshot().payload());

        var moved = authority.compareExchange(
            "zla1:a:1:a",
            ZLinkInMemoryLocationAuthority.Expectation.version(
                created.snapshot().storeVersion()),
            ZLinkInMemoryLocationAuthority.Mutation.newOwner(
                new byte[] {4}));
        assertEquals(
            created.snapshot().objectGeneration(),
            moved.snapshot().objectGeneration());
        assertNotEquals(
            created.snapshot().authorityOwnerGeneration(),
            moved.snapshot().authorityOwnerGeneration());
        assertEquals(
            ZLinkInMemoryLocationAuthority.CasKind.CONFLICT,
            authority.compareExchange(
                "zla1:a:1:a",
                ZLinkInMemoryLocationAuthority.Expectation.version(
                    created.snapshot().storeVersion()),
                ZLinkInMemoryLocationAuthority.Mutation.preserve(
                    new byte[] {5}))
                .kind());
        assertEquals(2, changes.size());
    }

    private static ZLinkServiceMailbox.Record record(
        String owner,
        ZLinkServiceMailbox.Domain domain,
        int bytes) {
        return new ZLinkServiceMailbox.Record(
            owner,
            domain,
            List.of(new byte[bytes]),
            null,
            null,
            null);
    }

    private static ZLinkServiceTopologyRegistry.Connection connection(
        String id,
        ZLinkServiceAdmissionGuard.ConnectionDirection direction) {
        return new ZLinkServiceTopologyRegistry.Connection(
            id,
            direction,
            direction.name() + ":" + id);
    }

    private static ZLinkServiceNodeDescriptor descriptor(
        String meshName,
        String rid,
        long lifecycle,
        long revision,
        List<ZLinkServiceNodeDescriptor.Channel> channels,
        int placementWeight) {
        return new ZLinkServiceNodeDescriptor(
            meshName,
            RoutingId.from(rid),
            lifecycle,
            revision,
            "inproc://" + rid,
            channels,
            ZLinkServiceNodeDescriptor.State.SERVING,
            "default",
            4 * 1024 * 1024,
            0,
            List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
            ZLinkServiceNodeDescriptor.ObjectRole.SERVER,
            placementWeight,
            100,
            10,
            0,
            0);
    }

    private static ZLinkServiceNodeDescriptor descriptorWithCapacity(
        String rid,
        int placementWeight,
        int activeLimit,
        int activeUsed,
        int pendingLimit,
        int pendingUsed) {
        return new ZLinkServiceNodeDescriptor(
            "mesh",
            RoutingId.from(rid),
            1,
            1,
            "inproc://" + rid,
            List.of(),
            ZLinkServiceNodeDescriptor.State.SERVING,
            "default",
            4 * 1024 * 1024,
            0,
            List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
            ZLinkServiceNodeDescriptor.ObjectRole.SERVER,
            placementWeight,
            activeLimit,
            pendingLimit,
            activeUsed,
            pendingUsed);
    }

    private static ZLinkServiceNodeDescriptor immutableMutation(
        ZLinkServiceNodeDescriptor current,
        String field) {
        return new ZLinkServiceNodeDescriptor(
            current.meshName(),
            current.nodeRoutingId(),
            current.lifecycleGeneration(),
            current.descriptorRevision() + 1,
            field.equals("endpoint")
                ? current.advertisedEndpoint() + "-changed"
                : current.advertisedEndpoint(),
            field.equals("channelSet")
                ? List.of(new ZLinkServiceNodeDescriptor.Channel(
                    "payments", 100))
                : current.channels(),
            current.state(),
            field.equals("securityIdentity")
                ? current.securityIdentity() + "-changed"
                : current.securityIdentity(),
            field.equals("effectiveMaxMessageBytes")
                ? current.effectiveMaxMessageBytes() + 1
                : current.effectiveMaxMessageBytes(),
            field.equals("applicationVersion")
                ? current.applicationVersion() + 1
                : current.applicationVersion(),
            field.equals("protocolCapabilities")
                ? List.of(
                    ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY,
                    "optional-v1")
                : current.protocolCapabilities(),
            field.equals("objectRole")
                ? ZLinkServiceNodeDescriptor.ObjectRole.CLIENT
                : current.objectRole(),
            current.placementWeight(),
            field.equals("activeCapacityLimit")
                ? current.activeCapacityLimit() + 1
                : current.activeCapacityLimit(),
            field.equals("pendingCapacityLimit")
                ? current.pendingCapacityLimit() + 1
                : current.pendingCapacityLimit(),
            current.activeCapacityUsed(),
            current.pendingCapacityUsed());
    }
}
