package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;
import systems.zlink.framework.channels.ZLinkRouteSendHandler;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.mesh.ZLinkMeshNodesRuntime;

import java.time.Duration;
import java.util.Arrays;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;

final class ZLinkJavaMeshNodeIsolationTest {
    private static final String MESH_A = "a";
    private static final String MESH_B = "b";
    private static final String LOOPBACK = "tcp://127.0.0.1:";
    private static final Duration GUARD = Duration.ofSeconds(5);
    private static final RoutingId SOURCE = RoutingId.from("isolation-source");
    private static final RoutingId TARGET = RoutingId.from("isolation-target");

    @Test
    void blockedNodeTurnDoesNotHoldAnotherNodesSubmissionOrReceivePump() throws Exception {
        var targetOptions = new DefaultZLinkFrameworkOptions();
        targetOptions
                .addRouteMesh(MESH_A)
                .listen(LOOPBACK + 0)
                .setRoutingId(TARGET)
                .addRouteSendHandler(Sink.class, Probe.class);
        targetOptions
                .addRouteMesh(MESH_B)
                .listen(LOOPBACK + 0)
                .setRoutingId(TARGET)
                .addRouteRequestHandler(Echo.class, Probe.class, String.class);
        try (var target = ZLinkFrameworkRuntimeTestAccess.start(targetOptions)) {
            String endpointA =
                    target.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH_A).endpoint();
            String endpointB =
                    target.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH_B).endpoint();
            var sourceOptions = new DefaultZLinkFrameworkOptions();
            for (String mesh : new String[] {MESH_A, MESH_B}) {
                var node =
                        sourceOptions.addRouteMesh(mesh).listen(LOOPBACK + 0).setRoutingId(SOURCE);
                node.peerConnections().connect(TARGET, mesh.equals(MESH_A) ? endpointA : endpointB);
                if (mesh.equals(MESH_B))
                    node.addRouteRequestHandler(Echo.class, Probe.class, String.class);
            }
            try (var source = ZLinkFrameworkRuntimeTestAccess.start(sourceOptions);
                    var callers = Executors.newVirtualThreadPerTaskExecutor()) {
                await(
                        () ->
                                source.routeMeshRuntime().snapshot(MESH_A).readyPeerCount() == 1
                                        && source.routeMeshRuntime()
                                                        .snapshot(MESH_B)
                                                        .readyPeerCount()
                                                == 1
                                        && target.routeMeshRuntime()
                                                        .snapshot(MESH_A)
                                                        .readyPeerCount()
                                                == 1
                                        && target.routeMeshRuntime()
                                                        .snapshot(MESH_B)
                                                        .readyPeerCount()
                                                == 1);
                assertEquals(
                        "warmup",
                        request(source, TARGET, "warmup")
                                .get(GUARD.toMillis(), TimeUnit.MILLISECONDS));
                assertEquals(
                        "reverse-warmup",
                        request(target, SOURCE, "reverse-warmup")
                                .get(GUARD.toMillis(), TimeUnit.MILLISECONDS));
                var entered = new CountDownLatch(1);
                var release = new CountDownLatch(1);
                var held =
                        lane(source, MESH_A)
                                .runAsync(
                                        () -> {
                                            entered.countDown();
                                            try {
                                                release.await();
                                            } catch (InterruptedException interrupted) {
                                                Thread.currentThread().interrupt();
                                                throw new AssertionError(interrupted);
                                            }
                                            return null;
                                        })
                                .toCompletableFuture();
                var sendingThread = new AtomicReference<Thread>();
                CompletableFuture<Void> sending = null;
                try {
                    if (!entered.await(GUARD.toMillis(), TimeUnit.MILLISECONDS))
                        throw new AssertionError("Node A did not enter its turn");
                    sending =
                            CompletableFuture.runAsync(
                                    () -> {
                                        sendingThread.set(Thread.currentThread());
                                        source.route()
                                                .sendToNode(MESH_A, TARGET, new Probe("load"))
                                                .submit()
                                                .toCompletableFuture()
                                                .join();
                                    },
                                    callers);
                    awaitSourceTurnWait(sendingThread);
                    var forward =
                            CompletableFuture.supplyAsync(
                                    () -> request(source, TARGET, "forward").join(), callers);
                    var reverse =
                            CompletableFuture.supplyAsync(
                                    () -> request(target, SOURCE, "reverse").join(), callers);
                    // The guard only terminates a deadlocked test. Progress is checked
                    // while A still owns its turn, without a latency threshold.
                    CompletableFuture.allOf(forward, reverse)
                            .get(GUARD.toMillis(), TimeUnit.MILLISECONDS);
                    assertFalse(held.isDone());
                    assertFalse(sending.isDone());
                    assertEquals("forward", forward.join());
                    assertEquals("reverse", reverse.join());
                } finally {
                    release.countDown();
                    held.get(GUARD.toMillis(), TimeUnit.MILLISECONDS);
                    if (sending != null) sending.get(GUARD.toMillis(), TimeUnit.MILLISECONDS);
                }
            }
        }
    }

    private static CompletableFuture<String> request(
            ZLinkFrameworkRuntime runtime, RoutingId target, String value) {
        return runtime.route()
                .requestToNode(MESH_B, target, new Probe(value))
                .submit(String.class)
                .toCompletableFuture();
    }

    private static void awaitSourceTurnWait(AtomicReference<Thread> caller) throws Exception {
        await(
                () -> {
                    Thread thread = caller.get();
                    return thread != null
                            && Arrays.stream(thread.getStackTrace())
                                    .anyMatch(
                                            frame ->
                                                    frame.getClassName()
                                                                    .equals(
                                                                            ZLinkStateLane.class
                                                                                    .getName())
                                                            && frame.getMethodName()
                                                                    .equals("awaitTurn"));
                });
    }

    private static void await(BooleanSupplier ready) throws Exception {
        long guard = System.nanoTime() + GUARD.toNanos();
        while (!ready.getAsBoolean()) {
            if (System.nanoTime() >= guard)
                throw new AssertionError("Isolation test prerequisite did not progress");
            Thread.sleep(1);
        }
    }

    private static ZLinkStateLane lane(ZLinkFrameworkRuntime runtime, String mesh)
            throws Exception {
        var nodesField = ZLinkFrameworkRuntime.class.getDeclaredField("meshNodes");
        nodesField.setAccessible(true);
        var nodes = (ZLinkMeshNodesRuntime) nodesField.get(runtime);
        var ownerField = ZLinkJavaRawMeshNode.class.getDeclaredField("transportStateLane");
        ownerField.setAccessible(true);
        return (ZLinkStateLane) ownerField.get(nodes.nodesByName().get(mesh));
    }

    public record Probe(String value) {}

    public static final class Echo implements ZLinkRouteRequestHandler<Probe, String> {
        @Override
        public CompletionStage<String> handle(Probe request, ZLinkRouteMessageContext context) {
            return CompletableFuture.completedFuture(request.value());
        }
    }

    public static final class Sink implements ZLinkRouteSendHandler<Probe> {
        @Override
        public CompletionStage<Void> handle(Probe message, ZLinkRouteMessageContext context) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
