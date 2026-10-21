package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ConnectResult;
import systems.zlink.contracts.errors.ZlinkConnectException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.internal.backend.ZLinkPeerIntentClosePendingException;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;
import java.util.function.LongSupplier;

final class ZLinkJavaLivenessNativeContractTest {
    @Test
    void nativeNotFoundDuringLivenessCleanupKeepsReceivePumpAlive() throws Exception {
        var expiredRid = RoutingId.from("liveness-expired");
        var sourceRid = RoutingId.from("liveness-source");
        var healthyRid = RoutingId.from("liveness-healthy");
        var offset = new AtomicLong();
        var armed = new AtomicBoolean();
        var clockEntered = new CompletableFuture<Void>();
        var releaseClock = new CompletableFuture<Void>();
        var sourceSocket = new AtomicReference<RouterSocket>();
        var sourcePump = new AtomicReference<Thread>();
        var previousHandler = new AtomicReference<Thread.UncaughtExceptionHandler>();
        var pumpFailure = new CompletableFuture<Throwable>();
        LongSupplier clock =
                () -> {
                    if (ZLinkStateLane.current() == null && armed.compareAndSet(true, false)) {
                        var thread = Thread.currentThread();
                        var previous = thread.getUncaughtExceptionHandler();
                        sourcePump.set(thread);
                        previousHandler.set(previous);
                        thread.setUncaughtExceptionHandler(
                                (failedThread, failure) -> {
                                    pumpFailure.complete(failure);
                                    if (previous != null)
                                        previous.uncaughtException(failedThread, failure);
                                });
                        clockEntered.complete(null);
                        releaseClock.join();
                    }
                    return System.nanoTime() + offset.get();
                };
        try (var context = Zlink.createContext()) {
            Context decorated =
                    (Context)
                            Proxy.newProxyInstance(
                                    Context.class.getClassLoader(),
                                    new Class<?>[] {Context.class},
                                    (proxy, method, args) -> {
                                        var result = invoke(method, context, args);
                                        if (method.getName().equals("createRouterSocket"))
                                            sourceSocket.set((RouterSocket) result);
                                        return result;
                                    });
            try (var source =
                            new ZLinkJavaRawMeshNode(
                                    decorated,
                                    "liveness-native",
                                    System::currentTimeMillis,
                                    clock,
                                    ZLinkServiceLivenessRegistry.DEFAULT_PROBE_INTERVAL,
                                    ZLinkServiceLivenessRegistry.DEFAULT_PEER_TIMEOUT);
                    var expired = new ZLinkJavaRawMeshNode(context, "liveness-native");
                    var healthy = new ZLinkJavaRawMeshNode(context, "liveness-native")) {
                configure(source, sourceRid);
                var expiredEndpoint = configure(expired, expiredRid);
                var healthyEndpoint = configure(healthy, healthyRid);
                var incoming = new LinkedBlockingQueue<Map.Entry<RoutingId, String>>();
                Consumer<ZLinkMeshDispatchRecord> receive =
                        record -> {
                            try (record) {
                                incoming.add(
                                        Map.entry(
                                                record.receive().sourceNodeRid(),
                                                record.parts().getFirst().toUtf8String()));
                            }
                        };
                source.startDispatch(receive);
                healthy.startDispatch(receive);
                source.start();
                expired.start();
                awaitAdmission(
                        source, expiredRid, () -> source.connectPeer(expiredEndpoint, expiredRid));
                awaitAdmission(expired, sourceRid, () -> {});
                var cleaned = new CompletableFuture<Void>();
                try (var observation =
                        source.onPeerStateChanged(
                                expiredRid,
                                () -> {
                                    if (!source.observePeerRoute(expiredRid).ready()) {
                                        offset.set(0);
                                        cleaned.complete(null);
                                    }
                                },
                                () -> {})) {
                    armed.set(true);
                    // Wake the existing receive owner with an ordinary record. Block its
                    // next clock read before enqueuing the public connection-close request.
                    try (var packet = Message.from("BeforeCleanup");
                            var payload = Message.from("{}")) {
                        expired.sendNode(sourceRid, null, List.of(packet, payload), false, null)
                                .toCompletableFuture()
                                .get(5, TimeUnit.SECONDS);
                    }
                    clockEntered.get(5, TimeUnit.SECONDS);
                    try {
                        assertThrows(
                                ZLinkPeerIntentClosePendingException.class,
                                () ->
                                        source.replacePeerConnection(
                                                expiredEndpoint,
                                                expiredRid,
                                                expired.lifecycleGeneration() + 1,
                                                "default"));
                        offset.set(ZLinkServiceLivenessRegistry.DEFAULT_PEER_TIMEOUT.toNanos());
                    } finally {
                        releaseClock.complete(null);
                    }
                    // The pump observes routes, drains this physical-close request, then
                    // expires liveness. Its stale admission therefore gets native NOT_FOUND.
                    CompletableFuture.anyOf(cleaned, pumpFailure).get(5, TimeUnit.SECONDS);
                    if (pumpFailure.isDone()) {
                        var failure = pumpFailure.join();
                        var result =
                                failure instanceof ZlinkConnectException connect
                                        ? connect.getResult().toString()
                                        : failure.toString();
                        fail(
                                "receive pump stopped during native liveness cleanup: " + result,
                                failure);
                    }
                } finally {
                    releaseClock.complete(null);
                }
                var missing =
                        assertThrows(
                                ZlinkConnectException.class,
                                () -> sourceSocket.get().disconnectRid(expiredRid));
                assertEquals(ConnectResult.NOT_FOUND, missing.getResult());
                assertFalse(source.observePeerRoute(expiredRid).ready());
                healthy.start();
                awaitAdmission(
                        source, healthyRid, () -> source.connectPeer(healthyEndpoint, healthyRid));
                awaitAdmission(healthy, sourceRid, () -> {});
                try (var packet = Message.from("LivenessAfterCleanup");
                        var payload = Message.from("{}")) {
                    source.sendNode(healthyRid, null, List.of(packet, payload), false, null)
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS);
                    healthy.sendNode(sourceRid, null, List.of(packet, payload), false, null)
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS);
                }
                var receivedFrom = new HashSet<RoutingId>();
                while (receivedFrom.size() < 2) {
                    var record = incoming.poll(5, TimeUnit.SECONDS);
                    assertNotNull(record, "both receive owners must deliver after cleanup");
                    if (record.getValue().equals("LivenessAfterCleanup"))
                        receivedFrom.add(record.getKey());
                }
                assertEquals(Set.of(sourceRid, healthyRid), receivedFrom);
            } finally {
                releaseClock.complete(null);
                var thread = sourcePump.get();
                if (thread != null && thread.isAlive())
                    thread.setUncaughtExceptionHandler(previousHandler.get());
            }
        }
    }

    @Test
    void nativeContextTerminationIsNotConsumedAsPeerAbsence() {
        try (var context = Zlink.createContext();
                var socket = context.createRouterSocket()) {
            context.shutdown();
            var failure =
                    assertThrows(
                            ZlinkConnectException.class,
                            () ->
                                    ZLinkJavaSocketSupport.disconnectRidIfPresent(
                                            socket, RoutingId.from("expired")));
            assertEquals(ConnectResult.INTERNAL_ERROR, failure.getResult());
        }
    }

    private static Object invoke(java.lang.reflect.Method method, Object target, Object[] args)
            throws Throwable {
        try {
            return method.invoke(target, args);
        } catch (InvocationTargetException failure) {
            throw failure.getCause();
        }
    }

    private static String configure(ZLinkJavaRawMeshNode node, RoutingId rid) {
        node.setRoutingId(rid);
        var endpoint = "inproc://liveness-native-" + rid + "-" + UUID.randomUUID();
        node.setBind(endpoint);
        return endpoint;
    }

    private static void awaitAdmission(ZLinkJavaRawMeshNode node, RoutingId rid, Runnable connect)
            throws Exception {
        var ready = new CompletableFuture<Void>();
        try (var observation =
                node.onPeerStateChanged(
                        rid,
                        () -> {
                            if (node.observePeerRoute(rid).ready()) ready.complete(null);
                        },
                        () -> {})) {
            connect.run();
            if (node.observePeerRoute(rid).ready()) ready.complete(null);
            ready.get(5, TimeUnit.SECONDS);
        }
    }
}
