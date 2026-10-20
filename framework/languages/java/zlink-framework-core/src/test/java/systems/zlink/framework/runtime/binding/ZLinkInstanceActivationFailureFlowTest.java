package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.*;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.diagnostics.*;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityReadResult;
import systems.zlink.framework.runtime.internal.service.*;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;

import java.util.*;
import java.util.concurrent.*;
import java.util.logging.*;

final class ZLinkInstanceActivationFailureFlowTest {
    @ParameterizedTest
    @CsvSource({
        "deadline,false",
        "rejected,false",
        "shutdown,false",
        "deadline,true",
        "rejected,true",
        "shutdown,true"
    })
    void detachedOneWayFailureRecordsOneDropWithOriginalFlow(String scenario, boolean remote)
            throws Exception {
        var records = new CopyOnWriteArrayList<String>();
        var recorded = new CompletableFuture<Void>();
        var logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        var previousLevel = logger.getLevel();
        var handler =
                new Handler() {
                    public void publish(LogRecord record) {
                        records.add(record.getMessage());
                        if (record.getMessage().contains("phase=dropped")) recorded.complete(null);
                    }

                    public void flush() {}

                    public void close() {}
                };
        logger.addHandler(handler);
        logger.setLevel(Level.ALL);
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "failure-flow");
                var peer = new ZLinkJavaRawMeshNode(context, "failure-flow")) {
            node.setRoutingId(RoutingId.from("failure-flow-node"));
            node.setBind("inproc://failure-flow-" + UUID.randomUUID());
            var options = new ZLinkDispatchOptionsRegistration();
            options.messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            node.setDispatchErrorReporter(
                    new ZLinkDispatchErrorReporter(
                            options, ZLinkHandlerActivator.reflection(), Runnable::run));
            node.start();
            ZLinkJavaRawMeshNode source = node;
            if (remote) {
                peer.setRoutingId(RoutingId.from("failure-flow-source"));
                peer.setBind("inproc://failure-flow-source-" + UUID.randomUUID());
                peer.start();
                peer.connectPeer(node.status().localEndpoint(), node.routingId());
                long guard = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
                while (peer.peers().stream()
                        .noneMatch(
                                p ->
                                        p.state()
                                                == systems.zlink.framework.runtime.internal.binding
                                                        .spot.MeshPeerState.ADMITTED)) {
                    assertTrue(System.nanoTime() < guard, "peer admission did not complete");
                    Thread.sleep(1);
                }
                source = peer;
            }
            var reserved = new CompletableFuture<ZLinkServiceM6BWireCodec.InstanceRouteFence>();
            var entered = new CompletableFuture<Void>();
            node.registerInstanceSpotType(
                    "room",
                    new ZLinkInternalMeshNode.InstanceSpotActivationHandler() {
                        public CompletionStage<ZLinkServiceM6BWireCodec.InstanceRouteFence> reserve(
                                ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope envelope,
                                ZLinkAuthorityReadResult authority) {
                            entered.complete(null);
                            return reserved;
                        }

                        public CompletionStage<Void> activate(
                                String type,
                                ZLinkServiceM6BWireCodec.InstanceRouteFence route,
                                ZLinkBackendSpot spot) {
                            throw new AssertionError("failed reservation cannot activate");
                        }
                    });
            var flow = ZLinkFlowContext.create(ZLinkFlowOrigin.APPLICATION);
            var header =
                    ZLinkChannelEnvelope.create(
                            ZLinkChannelEnvelope.KIND_COMMAND,
                            "",
                            "First",
                            "application/json",
                            null,
                            Map.of(),
                            flow);
            var route =
                    new ZLinkServiceM6BWireCodec.InstanceColdActivation(
                            node.routingId(),
                            node.lifecycleGeneration(),
                            "failed-room",
                            node.name(),
                            "room",
                            "1",
                            System.currentTimeMillis() + 100);
            try (var envelope = ZLinkChannelEnvelope.encodeHeader(header);
                    var body = Message.from("{}")) {
                source.submitInstanceSpotSend(
                                route, "room", null, new byte[0], List.of(envelope, body))
                        .toCompletableFuture()
                        .get(5, TimeUnit.SECONDS);
            }
            entered.get(5, TimeUnit.SECONDS);
            var kind =
                    switch (scenario) {
                        case "deadline" -> ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED;
                        case "shutdown" -> ZLinkFrameworkErrorKind.SHUTTING_DOWN;
                        default -> ZLinkFrameworkErrorKind.REJECTED;
                    };
            var failure = new ZLinkFrameworkException(kind, "activation " + scenario);
            reserved.completeExceptionally(failure);
            assertDoesNotThrow(() -> recorded.get(5, TimeUnit.SECONDS), () -> records.toString());
            ((ZLinkJavaRawSpotNode) node.spotNode())
                    .instanceActivationTerminals()
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            var drops = records.stream().filter(line -> line.contains("phase=dropped")).toList();
            assertEquals(1, drops.size(), records.toString());
            String drop = drops.getFirst();
            assertTrue(drop.contains("surface=instance_spot"), drop);
            assertTrue(
                    drop.contains(
                            "reason="
                                    + (scenario.equals("deadline")
                                            ? "activation_timeout"
                                            : "activation_rejected")),
                    drop);
            assertTrue(drop.contains(flow.flowId()), drop);
            assertTrue(drop.contains("origin=application"), drop);
            assertTrue(drop.contains("error_type=ZLinkFrameworkException"), drop);
            assertTrue(drop.contains("activation " + scenario), drop);
        } finally {
            logger.removeHandler(handler);
            logger.setLevel(previousLevel);
        }
    }
}
