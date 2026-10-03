package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.lang.reflect.Field;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;

class ZLinkJavaRawMeshNodeDescriptorOrderTest {
    /**
     * The admission response carries the local descriptor. A response read before markServiceReady
     * but submitted after its UPDATE reached the peer as a lower revision, the peer rejected it as
     * stale (wire-protocol §4 DescriptorRevision ordering), and the HELLO sender never recorded the
     * completed admission, so readyPeerCount stayed short by one. The response is therefore read
     * and submitted in the descriptor lane turn that also orders descriptor publication.
     */
    @Test
    void admissionResponseIsOrderedWithPendingDescriptorPublication() throws Exception {
        RoutingId targetRid = RoutingId.from("descriptor-order-target");
        String endpoint = "inproc://descriptor-order-" + UUID.randomUUID();
        CountDownLatch release = new CountDownLatch(1);
        try (var context = Zlink.createContext();
                var target = new ZLinkJavaRawMeshNode(context, "mesh");
                var source = new ZLinkJavaRawMeshNode(context, "mesh")) {
            target.setRoutingId(targetRid);
            target.setBind(endpoint);
            target.setObjectRole(ZLinkMeshNodeObjectRole.SERVER);
            target.addChannel("orders");
            target.setChannelWeight("orders", 100);
            target.deferServiceReadyPublication();
            source.setRoutingId(RoutingId.from("descriptor-order-source"));
            source.setBind("inproc://descriptor-order-source-" + UUID.randomUUID());
            target.start();
            source.start();

            ZLinkStateLane descriptorLane = descriptorLane(target);
            var blocked = new CountDownLatch(1);
            descriptorLane.runAsync(
                    () -> {
                        blocked.countDown();
                        awaitLatch(release);
                        return null;
                    });
            assertTrue(blocked.await(2, TimeUnit.SECONDS));
            CompletableFuture<Void> serving = CompletableFuture.runAsync(target::markServiceReady);

            source.connectPeer(endpoint, targetRid);
            // The target admits the HELLO, but its ADMIT waits for the descriptor lane.
            Thread.sleep(300);
            assertTrue(
                    source.peers().stream().noneMatch(p -> p.state() == MeshPeerState.ADMITTED),
                    "admission response left before the pending descriptor publication");

            release.countDown();
            serving.get(2, TimeUnit.SECONDS);
            await(() -> source.readyPeerCount() == 1);
            assertEquals(1, source.readyChannelMemberCount("orders"));
        } finally {
            release.countDown();
        }
    }

    private static ZLinkStateLane descriptorLane(ZLinkJavaRawMeshNode node) throws Exception {
        Field field = ZLinkJavaRawMeshNode.class.getDeclaredField("descriptorStateLane");
        field.setAccessible(true);
        return (ZLinkStateLane) field.get(node);
    }

    private static void awaitLatch(CountDownLatch latch) {
        try {
            assertTrue(latch.await(10, TimeUnit.SECONDS));
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException(interrupted);
        }
    }

    private static void await(BooleanSupplier condition) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (!condition.getAsBoolean()) {
            if (System.nanoTime() - deadline >= 0) {
                throw new AssertionError("runtime state transition was not observed");
            }
            Thread.yield();
        }
    }
}
