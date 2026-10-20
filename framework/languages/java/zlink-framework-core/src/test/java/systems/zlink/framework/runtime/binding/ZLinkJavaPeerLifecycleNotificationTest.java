package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.TimeUnit;

final class ZLinkJavaPeerLifecycleNotificationTest {
    @Test
    void lastLogicalRemovalEndsOnlyItsOwnPendingOperation() throws Exception {
        try (var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "retirement")) {
            start(source);
            var a = RoutingId.from("retiring-a");
            var b = RoutingId.from("retaining-b");
            var bObserved = new CompletableFuture<Void>();
            var aObserved = new CompletableFuture<Void>();
            var pendingB = request(source, b, bObserved);
            var pendingA = request(source, a, aObserved);
            try {
                bObserved.get(3, TimeUnit.SECONDS);
                aObserved.get(3, TimeUnit.SECONDS);
                source.observePeerAdmissionExpectation(
                                a,
                                "inproc://retiring-a",
                                8,
                                "security",
                                ZLinkMeshNodeObjectRole.NONE)
                        .toCompletableFuture()
                        .join();
                source.forgetPeerAdmissionExpectation(a).toCompletableFuture().join();
                var failure = assertThrows(CompletionException.class, pendingA::join);
                assertEquals(
                        ZLinkFrameworkErrorKind.UNAVAILABLE,
                        assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
                assertFalse(pendingB.isDone(), "Removing another target cannot end this operation");
            } finally {
                source.close();
                pendingA.handle((value, failure) -> null).join();
                pendingB.handle((value, failure) -> null).join();
            }
        }
    }

    @Test
    void terminalSnapshotExcludesLateAndUnregisteredObservers() throws Exception {
        try (var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "snapshot")) {
            start(source);
            var target = RoutingId.from("snapshot-target");
            var oldEnded = new CompletableFuture<Void>();
            var removedEnded = new CompletableFuture<Void>();
            var lateEnded = new CompletableFuture<Void>();
            var old = source.onPeerStateChanged(target, () -> {}, () -> oldEnded.complete(null));
            var removed =
                    source.onPeerStateChanged(target, () -> {}, () -> removedEnded.complete(null));
            var lane = source.descriptorStateLaneForTest();
            var deliveryBoundary = new CompletableFuture<Void>();
            var resumeDelivery = new CompletableFuture<Void>();
            AutoCloseable late = null;
            try {
                // Hold a lane turn until the observer set changes. An existing drain may
                // consume later work without submitting another task to its executor.
                assertTrue(
                        lane.tryPost(
                                () -> {
                                    deliveryBoundary.complete(null);
                                    return resumeDelivery;
                                }));
                deliveryBoundary.get(3, TimeUnit.SECONDS);
                var delivered = source.signalLifecycleEnded(target);
                removed.close();
                late = source.onPeerStateChanged(target, () -> {}, () -> lateEnded.complete(null));
                assertFalse(oldEnded.isDone(), "Terminal delivery must follow the publishing turn");
                resumeDelivery.complete(null);
                delivered.toCompletableFuture().get(3, TimeUnit.SECONDS);
                assertTrue(oldEnded.isDone());
                assertFalse(
                        removedEnded.isDone(), "Unregistered observers cannot consume the event");
                assertFalse(
                        lateEnded.isDone(), "A later observer cannot consume an earlier removal");
            } finally {
                resumeDelivery.complete(null);
                old.close();
                removed.close();
                if (late != null) late.close();
            }
        }
    }

    private static CompletableFuture<List<byte[]>> request(
            ZLinkJavaRawMeshNode source, RoutingId target, CompletableFuture<Void> observed) {
        return ZLinkJavaDurableRequest.request(
                        () -> List.of(new byte[] {1}),
                        (frames, remaining, admission) -> {
                            throw new AssertionError("An absent route cannot submit");
                        },
                        () -> source.observePeerRoute(target),
                        (changed, ended) ->
                                source.onPeerStateChanged(
                                        target,
                                        () -> {
                                            changed.run();
                                            ZLinkJavaDurableInitialPeerTest.runOwner(
                                                    source, () -> observed.complete(null));
                                        },
                                        ended),
                        action -> ZLinkJavaDurableInitialPeerTest.runOwner(source, action),
                        Duration.ofSeconds(5))
                .toCompletableFuture();
    }

    private static void start(ZLinkJavaRawMeshNode source) {
        source.setRoutingId(RoutingId.from("lifecycle-source"));
        source.setBind("inproc://lifecycle-source-" + java.util.UUID.randomUUID());
        source.start();
    }
}
