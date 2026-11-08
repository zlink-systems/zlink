package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.RepeatedTest;

import systems.zlink.framework.runtime.internal.locations.*;

import java.time.Duration;

final class ZLinkSessionOwnerLeaseFenceTest {
    private static void assertRawOwnerFence(
            ZLinkOwnerLeaseOutageRecoveryTest.Fixture f,
            boolean provider,
            long current,
            long previous,
            long rejected) {
        var data = new ZLinkP4RuntimeRecoveryTest.Fixture(provider);
        var actor = data.actor("actor", "entry", 1);
        try (var host =
                new systems.zlink.framework.runtime.binding.ZLinkSessionFenceTestHost(
                        data.node.rid(), actor)) {
            var node = host.sessionMeshNode();
            node.setPeerAuthorityResolver(
                    new ZLinkMeshPeerAuthorityResolver(
                            f.store.backing, Duration.ofSeconds(1), f.runtime));
            node.confirmLocalOwnerFence(
                    new systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode
                            .PeerAuthorityFence(node.routingId(), 1, "owner", current));
            assertTrue(node.matchesLocalSessionOwner(node.routingId(), 1, "owner", current));
            assertTrue(node.matchesLocalSessionOwner(node.routingId(), 1, "owner", previous));
            assertFalse(node.matchesLocalSessionOwner(node.routingId(), 1, "owner", rejected));
            assertFalse(node.matchesLocalSessionOwner(node.routingId(), -1, "owner", previous));
            assertFalse(node.matchesLocalSessionOwner(node.routingId(), 1, "other", previous));
            assertFalse(node.matchesLocalSessionOwner(data.node.rid(), 1, "owner", previous));
        }
    }

    @RepeatedTest(100)
    void onlyCurrentAndImmediatelyPreviousClaimOfThisLifecycleAreAcceptedWithoutStoreReads() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new ZLinkOwnerLeaseOutageRecoveryTest.Fixture(provider, true)) {
                long first = f.original.leaseGeneration();
                assertTrue(f.runtime.acceptsSessionOwnerLease("owner", first, first));
                f.outage();
                f.at(Duration.ofSeconds(35).toNanos());
                assertTrue(f.runtime.renewOwnerLeaseOnce().toCompletableFuture().join());
                long second = f.runtime.currentOwnerToken().leaseGeneration();
                int reads = f.store.reads;
                assertRawOwnerFence(f, provider, second, first, second + 1);
                assertTrue(f.runtime.acceptsSessionOwnerLease("owner", second, first));
                assertTrue(f.runtime.acceptsSessionOwnerLease("owner", second, second));
                assertFalse(f.runtime.acceptsSessionOwnerLease("other", second, first));
                assertFalse(f.runtime.acceptsSessionOwnerLease("owner", first, first));
                assertFalse(f.runtime.acceptsSessionOwnerLease("owner", second, second + 1));
                assertEquals(reads, f.store.reads);
                f.at(Duration.ofSeconds(70).toNanos());
                assertTrue(f.runtime.renewOwnerLeaseOnce().toCompletableFuture().join());
                long third = f.runtime.currentOwnerToken().leaseGeneration();
                reads = f.store.reads;
                assertRawOwnerFence(f, provider, third, second, first);
                assertTrue(f.runtime.acceptsSessionOwnerLease("owner", third, second));
                assertFalse(f.runtime.acceptsSessionOwnerLease("owner", third, first));
                assertEquals(reads, f.store.reads);
                f.at(Duration.ofSeconds(99).toNanos());
                assertTrue(f.runtime.acceptsSessionOwnerLease("owner", third, second));
                assertEquals(reads, f.store.reads);
                f.at(Duration.ofSeconds(100).toNanos());
                assertFalse(f.runtime.acceptsSessionOwnerLease("owner", third, second));
                assertEquals(reads, f.store.reads);
            }
            try (var restarted = new ZLinkOwnerLeaseOutageRecoveryTest.Fixture(provider, true)) {
                assertNull(restarted.runtime.recoveryPreviousOwnerToken());
                assertFalse(restarted.runtime.acceptsSessionOwnerLease("owner", 1, 2));
            }
        }
    }
}
