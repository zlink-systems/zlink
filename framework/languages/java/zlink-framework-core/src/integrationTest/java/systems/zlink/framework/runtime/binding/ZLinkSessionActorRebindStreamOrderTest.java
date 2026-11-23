package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import static systems.zlink.framework.testsupport.ZLinkStreamTestFrames.readSkippingHeartbeatPings;

import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.api.RepetitionInfo;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.runtime.actors.ZLinkSessionActorsRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.messaging.ZLinkStringMessageSerializer;
import systems.zlink.framework.runtime.streams.ZLinkStreamActorControl;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.testsupport.ZLinkStreamTestFrames.Frame;
import systems.zlink.stream.connector.*;

import java.io.ByteArrayOutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;

final class ZLinkSessionActorRebindStreamOrderTest {
    private static final Duration TIMEOUT = Duration.ofSeconds(5);
    private static final String MESH = "mesh";
    private static final String ACTOR = "player";
    private static final String OLD_SLOT_PROBE = "old-slot-probe";
    private static final String UNSLOTTED_PROBE = "unslotted-probe";

    @RepeatedTest(40)
    void sendsUnboundBeforeReplacementBoundAndPush(RepetitionInfo repetition) throws Exception {
        var ready = new CompletableFuture<RoutingId>();
        try (var executor = Executors.newVirtualThreadPerTaskExecutor()) {
            verifyRebind(new EgressGate(executor), ready, repetition);
        }
    }

    private static void verifyRebind(
            EgressGate gate, CompletableFuture<RoutingId> ready, RepetitionInfo repetition)
            throws Exception {
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, MESH);
                var stream =
                        new ZLinkJavaStreamSocket(
                                context.createStreamSocket(), node, new ZLinkStateLane(gate));
                var relay = new ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"));
                var create = Message.from("create")) {
            node.setRoutingId(RoutingId.from("rebind-order"));
            node.setBind("inproc://rebind-order");
            node.start();
            ZLinkTestSessionOwners.install(node);
            var spots = (ZLinkJavaRawSpotNode) node.spotNode();
            var old = spots.createActor(ACTOR, 1, create);
            stream.startSessionService();
            stream.onTransportError(
                    (rid, event, code, message) -> {
                        if (event == MonitorEventType.CONNECTION_READY) ready.complete(rid);
                    });
            stream.bind("tcp://127.0.0.1:0");

            var endpoint = URI.create(stream.lastEndpoint());
            var clientEndpoint = URI.create("tcp://127.0.0.1:" + relay.getLocalPort());
            var connector = newConnector(clientEndpoint);
            var firstBound = new CompletableFuture<ZLinkStreamActor>();
            var disconnected = new CompletableFuture<ZLinkStreamDisconnected>();
            var error = new CompletableFuture<ZLinkStreamError>();
            var before = new CompletableFuture<String>();
            var after = new CompletableFuture<String>();
            connector.onActorBound(actor -> complete(firstBound, actor));
            connector.onDisconnected(event -> complete(disconnected, event));
            connector.onErrorReceived(event -> complete(error, event));
            connector.on("before", packet -> complete(before, packet.actorId()));
            connector.on("after", packet -> complete(after, packet.actorId()));
            try (var upstream = new Socket(endpoint.getHost(), endpoint.getPort())) {
                upstream.setSoTimeout((int) TIMEOUT.toMillis());
                relay.setSoTimeout((int) TIMEOUT.toMillis());
                await(connector.connect().submit());
                try (var downstream = relay.accept()) {
                    var rid = await(ready);
                    var session =
                            new ZLinkSessionActorsRuntime(
                                    spots,
                                    stream,
                                    rid,
                                    null,
                                    new ZLinkStringMessageSerializer(),
                                    null,
                                    true,
                                    ZLinkStreamCodec.RAW);
                    var original = await(session.bind(ref(old)));
                    var initial = readSkippingHeartbeatPings(upstream);
                    assertControl(
                            initial,
                            ZLinkStreamActorControl.BOUND,
                            ZLinkStreamActorControl.bound(1, ACTOR));
                    forward(downstream, initial);
                    var handle = await(firstBound);
                    assertEquals(ACTOR, handle.actorId());

                    try (var frame = pushFrame("before")) {
                        assertTrue(
                                spots.sendLocalActorBoundSession(
                                        old, List.of(frame), SendFlags.DONT_WAIT));
                    }
                    var beforeFrame = readSkippingHeartbeatPings(upstream);
                    assertEquals(1, beforeFrame.header().actorSlot().orElseThrow());
                    forward(downstream, beforeFrame);
                    assertEquals(ACTOR, await(before));
                    assertEquals(ZLinkStreamConnectionState.CONNECTED, connector.state());

                    ZLinkBackendActorRef next;
                    ZLinkSessionActor replacement;
                    boolean bindWaitedForOldSlot;
                    try (var unslotted = Message.from(UNSLOTTED_PROBE)) {
                        var releaseOldSlot = gate.hold(() -> sendPush(spots, old, OLD_SLOT_PROBE));
                        try {
                            var probeHeader =
                                    new ZLinkStreamHeader("lane-probe", Map.of(), Optional.empty());
                            assertTrue(
                                    stream.send(
                                            rid,
                                            probeHeader,
                                            List.of(unslotted),
                                            SendFlags.DONT_WAIT));
                            assertEquals(
                                    "lane-probe",
                                    readSkippingHeartbeatPings(upstream).header().name());

                            // Hold the slotless egress too. An unbound sent there cannot race
                            // ahead of the new bound and hide the regression.
                            var releaseUnslotted =
                                    gate.hold(
                                            () ->
                                                    stream.sendAsync(
                                                            rid,
                                                            new ZLinkStreamHeader(
                                                                    UNSLOTTED_PROBE,
                                                                    Map.of(),
                                                                    Optional.empty()),
                                                            List.of(unslotted)));
                            try {
                                await(spots.destroyActor(old, TIMEOUT));
                                next = spots.createActor(ACTOR, 2, create);
                                var pending = session.bind(ref(next));
                                bindWaitedForOldSlot = !pending.toCompletableFuture().isDone();
                                releaseOldSlot.run();
                                replacement = await(pending);
                            } finally {
                                releaseUnslotted.run();
                            }
                        } finally {
                            releaseOldSlot.run();
                        }
                    }
                    assertNotSame(original, replacement);
                    assertEquals(replacement, session.find(ACTOR).orElseThrow());

                    await(sendPush(spots, next, "after"));
                    var wire = new ArrayList<Frame>(5);
                    for (int i = 0; i < 5; i++) {
                        wire.add(readSkippingHeartbeatPings(upstream));
                    }
                    var actorFrames =
                            wire.stream()
                                    .filter(frame -> !UNSLOTTED_PROBE.equals(frame.header().name()))
                                    .toList();
                    assertEquals(
                            List.of(
                                    OLD_SLOT_PROBE,
                                    ZLinkStreamActorControl.UNBOUND,
                                    ZLinkStreamActorControl.BOUND,
                                    "after"),
                            actorFrames.stream().map(frame -> frame.header().name()).toList());
                    var first = actorFrames.get(1);
                    var second = actorFrames.get(2);
                    var afterFrame = actorFrames.get(3);
                    assertEquals(1, actorFrames.getFirst().header().actorSlot().orElseThrow());
                    assertControl(
                            first,
                            ZLinkStreamActorControl.UNBOUND,
                            ZLinkStreamActorControl.unbound(1));
                    assertControl(
                            second,
                            ZLinkStreamActorControl.BOUND,
                            ZLinkStreamActorControl.bound(2, ACTOR));
                    assertEquals(2, afterFrame.header().actorSlot().orElseThrow());
                    assertEquals("after", afterFrame.header().name());
                    assertTrue(bindWaitedForOldSlot);

                    // Relay the controls and new push in the order observed on the Core TCP peer.
                    forward(downstream, first, second, afterFrame);
                    assertEquals(ACTOR, await(after));
                    assertEquals(ZLinkStreamConnectionState.CONNECTED, connector.state());
                    assertFalse(handle.isBound());
                    assertFalse(disconnected.isDone());
                    assertFalse(error.isDone());
                    System.out.printf(
                            "rebind %d wire=%s,%s connector=%s pushReceived=true%n",
                            repetition.getCurrentRepetition(),
                            first.header().name(),
                            second.header().name(),
                            connector.state());
                }
            } finally {
                await(connector.close().submit());
            }
        }
    }

    private static ZLinkStreamConnector newConnector(URI endpoint) {
        return ZLinkStreamConnectorFactory.create(
                new ZLinkStreamConnectorOptions(
                        endpoint,
                        ZLinkStreamDispatchMode.IMMEDIATE,
                        Duration.ofSeconds(30),
                        3,
                        TIMEOUT,
                        65536,
                        false,
                        Duration.ofSeconds(1),
                        TIMEOUT,
                        false,
                        Duration.ofMillis(250),
                        TIMEOUT,
                        2.0));
    }

    private static ActorRef ref(ZLinkBackendActorRef actor) {
        return new ActorRef(actor.actorId(), actor.generation(), MESH, actor.nodeRid());
    }

    private static CompletionStage<Void> sendPush(
            ZLinkJavaRawSpotNode spots, ZLinkBackendActorRef actor, String name) {
        try (var frame = pushFrame(name)) {
            return spots.sendLocalActorBoundSessionAsync(actor, List.of(frame));
        }
    }

    private static Message pushFrame(String name) {
        var header = new ZLinkStreamHeader(name, Map.of(), Optional.empty());
        return Message.from(
                ZLinkStreamFrameCodec.encode(header, name.getBytes(StandardCharsets.UTF_8)));
    }

    private static final class EgressGate implements Executor {
        private final Executor executor;
        private final Thread caller = Thread.currentThread();
        private CompletableFuture<Runnable> held;

        EgressGate(Executor executor) {
            this.executor = executor;
        }

        @Override
        public void execute(Runnable task) {
            if (Thread.currentThread() == caller && held != null && held.complete(task)) {
                return;
            }
            executor.execute(task);
        }

        Runnable hold(Supplier<CompletionStage<Void>> send) throws Exception {
            var scheduled = new CompletableFuture<Runnable>();
            held = scheduled;
            try {
                assertFalse(send.get().toCompletableFuture().isDone());
                var task = await(scheduled);
                var release = new CompletableFuture<Void>();
                release.thenRun(() -> executor.execute(task));
                return () -> release.complete(null);
            } finally {
                held = null;
            }
        }
    }

    private static void assertControl(Frame frame, String name, byte[] body) {
        assertEquals(name, frame.header().name());
        assertArrayEquals(body, frame.body());
    }

    private static void forward(Socket downstream, Frame... frames) throws Exception {
        var output = new ByteArrayOutputStream();
        for (var frame : frames) {
            output.write(ZLinkStreamFrameCodec.encode(frame.headerBytes(), frame.body()));
        }
        output.writeTo(downstream.getOutputStream());
    }

    private static <T> T await(CompletionStage<T> stage) throws Exception {
        return stage.toCompletableFuture().get(TIMEOUT.toMillis(), TimeUnit.MILLISECONDS);
    }

    private static <T> CompletionStage<Void> complete(CompletableFuture<T> future, T value) {
        future.complete(value);
        return CompletableFuture.completedFuture(null);
    }
}
