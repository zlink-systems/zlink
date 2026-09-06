package systems.zlink.framework.runtime.channels;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.ArrayDeque;
import java.util.Deque;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.locations.ZLinkLocationPage;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkMonitoringBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitorEvent;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;

final class ZLinkClientServerM6ARuntimeTest {
    @Test
    void serviceWirePreservesOpaqueNonUtf8RoutingIdBytes() {
        RoutingId rid = RoutingId.from(new byte[] {
            0x00, (byte) 0xff, (byte) 0x80, 0x41
        });
        ZLinkClientServerServerDescriptor descriptor =
            descriptor("orders", rid, 7, 11, "tcp://127.0.0.1:7001", 25);

        byte[] encoded =
            ZLinkClientServerServiceWire.encodeAdmit(
                descriptor, 1024 * 1024);
        ZLinkClientServerServiceWire.Admit decoded =
            (ZLinkClientServerServiceWire.Admit)
                ZLinkClientServerServiceWire.decode(encoded);

        assertArrayEquals(
            rid.toBytes(), decoded.admission().serverRid().toBytes());
        assertEquals(7, decoded.admission().lifecycleGeneration());
        assertEquals(11, decoded.admission().descriptorRevision());
    }

    @Test
    void serviceWireRoundTripsExactLivenessProbeAndAck() {
        ZLinkClientServerServiceWire.LivenessProbe probe =
            (ZLinkClientServerServiceWire.LivenessProbe)
                ZLinkClientServerServiceWire.decode(
                    ZLinkClientServerServiceWire
                        .encodeLivenessProbe(91));
        ZLinkClientServerServiceWire.LivenessAck ack =
            (ZLinkClientServerServiceWire.LivenessAck)
                ZLinkClientServerServiceWire.decode(
                    ZLinkClientServerServiceWire
                        .encodeLivenessAck(91));
        assertEquals(91, probe.probeId());
        assertEquals(91, ack.probeId());
    }

    @Test
    void reconnectAdmissionFenceRejectsPreviousPhysicalPipeReply() {
        ZLinkChannelSocketRegistry sockets =
            new ZLinkChannelSocketRegistry();
        ControlledDealer dealer = new ControlledDealer();
        ZLinkClientServerServerDescriptor value =
            descriptor(
                "orders", RoutingId.from("server"), 7, 1,
                "tcp://127.0.0.1:7001", 100);
        sockets.addClientServerConnection("manual", value, dealer);
        ZLinkChannelSocketRegistry.AdmissionFence oldFence =
            sockets.clientServerTransportReady("manual");
        sockets.clientServerTransportTerminated("manual");
        ZLinkChannelSocketRegistry.AdmissionFence currentFence =
            sockets.clientServerTransportReady("manual");

        assertFalse(sockets.admitClientServerConnection(
            "manual", value, oldFence));
        assertNull(sockets.clientForOutbound("orders"));
        assertTrue(sockets.admitClientServerConnection(
            "manual", value, currentFence));
        assertSame(dealer, sockets.clientForOutbound("orders"));
    }

    @Test
    void stalePhysicalCleanupCannotRemoveReplacementWithSameConnectionId() {
        ZLinkChannelSocketRegistry sockets =
            new ZLinkChannelSocketRegistry();
        ZLinkClientServerServerDescriptor value =
            descriptor(
                "orders", RoutingId.from("server"), 7, 1,
                "tcp://127.0.0.1:7001", 100);
        ZLinkBackendDealerSocket oldDealer = dealer("old");
        ZLinkBackendDealerSocket newDealer = dealer("new");

        sockets.addClientServerConnection("same", value, oldDealer);
        sockets.admitClientServerConnection("same", value);
        sockets.addClientServerConnection("same", value, newDealer);
        sockets.admitClientServerConnection("same", value);

        sockets.removeClientServerConnection("same", oldDealer);
        sockets.reconnectClientServerConnection("same", oldDealer);

        assertSame(newDealer, sockets.clientForOutbound("orders"));
    }

    @Test
    void clientLivenessAndPushedUpdateAreConnectionFenced() {
        ZLinkChannelSocketRegistry sockets =
            new ZLinkChannelSocketRegistry();
        ControlledDealer dealer = new ControlledDealer();
        ZLinkClientServerServerDescriptor value =
            descriptor(
                "orders", RoutingId.from("server"), 7, 1,
                "tcp://127.0.0.1:7001", 100);
        sockets.addClientServerConnection("automatic", value, dealer);
        ZLinkChannelSocketRegistry.AdmissionFence fence =
            sockets.clientServerTransportReady("automatic");
        assertTrue(sockets.admitClientServerConnection(
            "automatic", value, fence));

        long base = System.nanoTime();
        sockets.tickClientServerLiveness(
            base + TimeUnit.SECONDS.toNanos(6));
        ZLinkClientServerServiceWire.LivenessProbe probe =
            (ZLinkClientServerServiceWire.LivenessProbe)
                ZLinkClientServerServiceWire.decode(
                    dealer.sent.get(0));
        assertTrue(dealer.requests.isEmpty());
        dealer.inbound.add(received(
            ZLinkClientServerServiceWire.encodeLivenessAck(
                probe.probeId() + 1)));
        sockets.tickClientServerLiveness(
            base + TimeUnit.SECONDS.toNanos(16));
        assertNull(sockets.clientForOutbound("orders"));

        ZLinkChannelSocketRegistry.AdmissionFence nextFence =
            sockets.clientServerTransportReady("automatic");
        assertTrue(sockets.admitClientServerConnection(
            "automatic", value, nextFence));
        long nextBase = System.nanoTime();
        sockets.tickClientServerLiveness(
            nextBase + TimeUnit.SECONDS.toNanos(6));
        ZLinkClientServerServiceWire.LivenessProbe nextProbe =
            (ZLinkClientServerServiceWire.LivenessProbe)
                ZLinkClientServerServiceWire.decode(dealer.sent.get(1));
        dealer.inbound.add(received(
            ZLinkClientServerServiceWire.encodeLivenessAck(
                nextProbe.probeId())));
        sockets.tickClientServerLiveness(System.nanoTime());
        assertSame(dealer, sockets.clientForOutbound("orders"));

        ZLinkClientServerServerDescriptor updated =
            descriptor(
                "orders", value.serverRid(), 7, 2,
                value.endpoint(), 25);
        dealer.inbound.add(received(
            ZLinkClientServerServiceWire.encodeUpdate(
                updated, Integer.MAX_VALUE)));
        sockets.tickClientServerLiveness(System.nanoTime());
        assertEquals(
            25,
            sockets.clientServerConnectionDescriptor(
                "automatic").weight());

        ZLinkClientServerServerDescriptor conflict =
            descriptor(
                "orders", value.serverRid(), 7, 2,
                value.endpoint(), 50);
        dealer.inbound.add(received(
            ZLinkClientServerServiceWire.encodeUpdate(
                conflict, Integer.MAX_VALUE)));
        sockets.tickClientServerLiveness(System.nanoTime());
        assertNull(sockets.clientForOutbound("orders"));
    }

    @Test
    void lifecycleConnectionIdUsesCanonicalRawRoutingIdBytes() {
        ZLinkClientServerServerDescriptor textRid =
            descriptor(
                "orders",
                RoutingId.from("1234"),
                9,
                1,
                "tcp://127.0.0.1:7001",
                50);
        ZLinkClientServerServerDescriptor integerRid =
            descriptor(
                "orders",
                RoutingId.from(1234L),
                9,
                1,
                "tcp://127.0.0.1:7001",
                50);

        assertEquals(
            textRid.serverRid().toString(),
            integerRid.serverRid().toString());
        assertNotEquals(
            ZLinkClientServerLocationRuntime.connectionId(textRid),
            ZLinkClientServerLocationRuntime.connectionId(integerRid));
    }

    @Test
    void weightedSelectionUsesOnlyAdmittedServingConnections() {
        ZLinkChannelSocketRegistry sockets =
            new ZLinkChannelSocketRegistry();
        ZLinkBackendDealerSocket first = dealer("first");
        ZLinkBackendDealerSocket second = dealer("second");
        ZLinkClientServerServerDescriptor firstDescriptor =
            descriptor(
                "orders", RoutingId.from("first"), 1, 1,
                "tcp://127.0.0.1:7001", 100);
        ZLinkClientServerServerDescriptor secondDescriptor =
            descriptor(
                "orders", RoutingId.from("second"), 1, 1,
                "tcp://127.0.0.1:7002", 300);
        sockets.addClientServerConnection("first", firstDescriptor, first);
        sockets.addClientServerConnection("second", secondDescriptor, second);

        assertNull(sockets.clientForOutbound("orders"));
        sockets.admitClientServerConnection("first", firstDescriptor);
        sockets.admitClientServerConnection("second", secondDescriptor);

        int firstSelections = 0;
        int secondSelections = 0;
        for (int index = 0; index < 400; index++) {
            ZLinkBackendDealerSocket selected =
                sockets.clientForOutbound("orders");
            if (selected == first) {
                firstSelections++;
            } else if (selected == second) {
                secondSelections++;
            }
        }
        assertEquals(100, firstSelections);
        assertEquals(300, secondSelections);
    }

    @Test
    void lateOldLifecycleRemovalCannotRemoveNewReadyConnection() {
        ZLinkChannelSocketRegistry sockets =
            new ZLinkChannelSocketRegistry();
        RoutingId rid = RoutingId.from(new byte[] {
            0x01, (byte) 0xf0, 0x02
        });
        ZLinkBackendDealerSocket oldDealer = dealer("old");
        ZLinkBackendDealerSocket newDealer = dealer("new");
        ZLinkClientServerServerDescriptor oldDescriptor =
            descriptor(
                "orders", rid, 10, 1,
                "tcp://127.0.0.1:7001", 100);
        ZLinkClientServerServerDescriptor newDescriptor =
            descriptor(
                "orders", rid, 11, 1,
                "tcp://127.0.0.1:7001", 100);
        sockets.addClientServerConnection(
            "orders/old", oldDescriptor, oldDealer);
        sockets.admitClientServerConnection(
            "orders/old", oldDescriptor);
        sockets.addClientServerConnection(
            "orders/new", newDescriptor, newDealer);

        assertSame(oldDealer, sockets.clientForOutbound("orders"));
        sockets.admitClientServerConnection(
            "orders/new", newDescriptor);
        sockets.removeClientServerConnection("orders/old");

        assertSame(newDealer, sockets.clientForOutbound("orders"));
    }

    @Test
    void manualAndAutomaticAliasOneTargetThenSourceRemovalKeepsOtherReady() {
        ZLinkChannelSocketRegistry sockets =
            new ZLinkChannelSocketRegistry();
        ZLinkBackendDealerSocket automatic = dealer("automatic");
        ZLinkBackendDealerSocket manual = dealer("manual");
        ZLinkClientServerServerDescriptor value =
            descriptor(
                "orders", RoutingId.from("server"), 7, 1,
                "tcp://127.0.0.1:7001", 100);
        sockets.addClientServerConnection(
            "z-automatic", value, automatic);
        sockets.addClientServerConnection(
            "a-manual", value, manual);
        sockets.admitClientServerConnection("z-automatic", value);
        sockets.admitClientServerConnection("a-manual", value);

        assertEquals(1, sockets.clientServerPhysicalConnectionCount());
        assertSame(automatic, sockets.clientForOutbound("orders"));
        sockets.removeClientServerConnection("a-manual");
        assertSame(automatic, sockets.clientForOutbound("orders"));
        assertEquals(1, sockets.clientServerPhysicalConnectionCount());

        ZLinkChannelSocketRegistry reverse =
            new ZLinkChannelSocketRegistry();
        ZLinkBackendDealerSocket reverseManual = dealer("manual-first");
        ZLinkBackendDealerSocket reverseAutomatic =
            dealer("automatic-second");
        reverse.addClientServerConnection(
            "a-manual", value, reverseManual);
        reverse.addClientServerConnection(
            "z-automatic", value, reverseAutomatic);
        reverse.admitClientServerConnection("a-manual", value);
        reverse.admitClientServerConnection("z-automatic", value);

        assertEquals(1, reverse.clientServerPhysicalConnectionCount());
        assertSame(reverseManual, reverse.clientForOutbound("orders"));
        reverse.removeClientServerConnection("a-manual");
        assertSame(reverseManual, reverse.clientForOutbound("orders"));
        assertEquals(1, reverse.clientServerPhysicalConnectionCount());
    }

    @Test
    void reservedHelloIsConsumedBeforeApplicationDispatch() {
        ZLinkChannelSocketRegistry sockets =
            new ZLinkChannelSocketRegistry();
        ZLinkClientServerServerDescriptor descriptor =
            descriptor(
                "orders", RoutingId.from("server"), 5, 2,
                "tcp://127.0.0.1:7001", 80);
        sockets.setClientServerServerDescriptor("orders", descriptor);
        byte[] hello = ZLinkClientServerServiceWire.encodeHello(
            new ZLinkClientServerServiceWire.Hello(
                "orders", "default", 4096));
        List<Message> reply = new ArrayList<>();
        Message request = Message.from(hello);
        ZLinkBackendReceived received = new ZLinkBackendReceived(
            ZLinkBackendRequestResult.OK,
            Optional.of(RoutingId.from("client")),
            Optional.empty(),
            Optional.of(1L),
            List.of(request),
            parts -> {
                for (Message part : parts) {
                    reply.add(Message.from(part));
                }
            },
            () -> { });

        assertTrue(sockets.tryHandleClientServerControl(
            "orders", router(), received));
        assertEquals(1, reply.size());
        try (Message response = reply.get(0)) {
            ZLinkClientServerServiceWire.Admit admit =
                (ZLinkClientServerServiceWire.Admit)
                    ZLinkClientServerServiceWire.decode(
                        response.toByteArray());
            assertEquals(
                descriptor.serverRid(), admit.admission().serverRid());
        }
    }

    @Test
    void reservedHelloUsesOpaqueDirectReplyPath() {
        ZLinkChannelSocketRegistry sockets =
            new ZLinkChannelSocketRegistry();
        ZLinkClientServerServerDescriptor descriptor =
            descriptor(
                "orders", RoutingId.from("server"), 5, 2,
                "tcp://127.0.0.1:7001", 80);
        sockets.setClientServerServerDescriptor("orders", descriptor);
        byte[] hello = ZLinkClientServerServiceWire.encodeHello(
            new ZLinkClientServerServiceWire.Hello(
                "orders", "default", 4096));
        List<Message> reply = new ArrayList<>();
        ControlledRouter router = new ControlledRouter();
        ZLinkBackendReceived received = new ZLinkBackendReceived(
            ZLinkBackendRequestResult.OK,
            Optional.of(RoutingId.from("client")),
            Optional.empty(),
            Optional.empty(),
            List.of(Message.from(hello)),
            parts -> {
                for (Message part : parts) {
                    reply.add(Message.from(part));
                }
            },
            () -> { });

        assertTrue(sockets.tryHandleClientServerControl(
            "orders", router, received));
        assertTrue(router.disconnected.isEmpty());
        assertEquals(1, reply.size());
        try (Message response = reply.get(0)) {
            ZLinkClientServerServiceWire.Admit admit =
                (ZLinkClientServerServiceWire.Admit)
                    ZLinkClientServerServiceWire.decode(
                        response.toByteArray());
            assertEquals(
                descriptor.serverRid(), admit.admission().serverRid());
        }
    }

    @Test
    void livenessProbeUsesOpaqueDirectReplyPath() {
        ZLinkChannelSocketRegistry sockets =
            new ZLinkChannelSocketRegistry();
        List<Message> reply = new ArrayList<>();
        ControlledRouter router = new ControlledRouter();
        ZLinkBackendReceived received = new ZLinkBackendReceived(
            ZLinkBackendRequestResult.OK,
            Optional.of(RoutingId.from("client")),
            Optional.empty(),
            Optional.empty(),
            List.of(Message.from(
                ZLinkClientServerServiceWire.encodeLivenessProbe(91))),
            parts -> {
                for (Message part : parts) {
                    reply.add(Message.from(part));
                }
            },
            () -> { });

        assertTrue(sockets.tryHandleClientServerControl(
            "orders", router, received));
        assertTrue(router.sent.isEmpty());
        assertTrue(router.disconnected.isEmpty());
        assertEquals(1, reply.size());
        try (Message response = reply.get(0)) {
            ZLinkClientServerServiceWire.LivenessAck ack =
                (ZLinkClientServerServiceWire.LivenessAck)
                    ZLinkClientServerServiceWire.decode(
                        response.toByteArray());
            assertEquals(91, ack.probeId());
        }
    }

    @Test
    void descriptorProjectionIgnoresPeerThatLostAdmission() {
        ZLinkChannelSocketRegistry sockets =
            new ZLinkChannelSocketRegistry();
        ZLinkClientServerServerDescriptor initial =
            descriptor(
                "orders", RoutingId.from("server"), 5, 1,
                "tcp://127.0.0.1:7001", 80);
        sockets.setClientServerServerDescriptor("orders", initial);
        ControlledRouter router = new ControlledRouter();
        Message hello = Message.from(
            ZLinkClientServerServiceWire.encodeHello(
                new ZLinkClientServerServiceWire.Hello(
                    "orders", "default", 4096)));
        assertTrue(sockets.tryHandleClientServerControl(
            "orders",
            router,
            new ZLinkBackendReceived(
                Optional.of(RoutingId.from("client")),
                Optional.empty(),
                Optional.of(1L),
                List.of(hello),
                parts -> { },
                () -> { })));

        router.sendFailure = new ZlinkSubmitException(
            SubmitResult.NOT_ADMITTED);
        ZLinkClientServerServerDescriptor draining =
            descriptor(
                "orders", RoutingId.from("server"), 5, 2,
                "tcp://127.0.0.1:7001", 0);

        assertDoesNotThrow(() ->
            sockets.setClientServerServerDescriptor(
                "orders", draining));
    }

    @Test
    void serverLivenessFencesAckByRoutingIdAndDisconnectsTimedOutPeer() {
        ZLinkChannelSocketRegistry sockets =
            new ZLinkChannelSocketRegistry();
        ZLinkClientServerServerDescriptor value =
            descriptor(
                "orders", RoutingId.from("server"), 5, 1,
                "tcp://127.0.0.1:7001", 100);
        sockets.setClientServerServerDescriptor("orders", value);
        ControlledRouter router = new ControlledRouter();
        RoutingId client = RoutingId.from("client-a");
        Message hello = Message.from(
            ZLinkClientServerServiceWire.encodeHello(
                new ZLinkClientServerServiceWire.Hello(
                    "orders", "default", 4096)));
        ZLinkBackendReceived received = new ZLinkBackendReceived(
            Optional.of(client),
            Optional.empty(),
            Optional.of(1L),
            List.of(hello),
            parts -> { });
        assertTrue(sockets.tryHandleClientServerControl(
            "orders", router, received));

        long base = System.nanoTime();
        sockets.tickClientServerLiveness(
            base + TimeUnit.SECONDS.toNanos(6));
        ZLinkClientServerServiceWire.LivenessProbe probe =
            (ZLinkClientServerServiceWire.LivenessProbe)
                ZLinkClientServerServiceWire.decode(
                    router.sent.get(0));
        router.acceptSend = false;
        sockets.tickClientServerLiveness(
            base + TimeUnit.SECONDS.toNanos(12));
        ZLinkClientServerServiceWire.LivenessProbe retry =
            (ZLinkClientServerServiceWire.LivenessProbe)
                ZLinkClientServerServiceWire.decode(
                    router.sent.get(1));
        assertEquals(probe.probeId(), retry.probeId());
        Message wrongAck = Message.from(
            ZLinkClientServerServiceWire.encodeLivenessAck(
                probe.probeId()));
        sockets.tryHandleClientServerControl(
            "orders",
            router,
            new ZLinkBackendReceived(
                Optional.of(RoutingId.from("client-b")),
                Optional.empty(),
                Optional.empty(),
                List.of(wrongAck)));
        sockets.tickClientServerLiveness(
            base + TimeUnit.SECONDS.toNanos(16));
        assertEquals(List.of(client), router.disconnected);
    }

    @Test
    void sameProcessServerUsesStoreDiscoveryAndExactDealerRouterAdmission() {
        ZLinkClientServerServerDescriptor descriptor =
            descriptor(
                "orders",
                RoutingId.from(new byte[] {
                    0x05, (byte) 0xff, 0x22
                }),
                8,
                3,
                "tcp://127.0.0.1:7010",
                70);
        ZLinkChannelSocketRegistry sockets =
            new ZLinkChannelSocketRegistry();
        sockets.setClientServerServerDescriptor("orders", descriptor);
        ZLinkBackendDealerSocket dealer =
            admittingDealer("automatic", descriptor);
        ZLinkChannelBackendAdapter adapter =
            (ZLinkChannelBackendAdapter) Proxy.newProxyInstance(
                ZLinkChannelBackendAdapter.class.getClassLoader(),
                new Class<?>[] {ZLinkChannelBackendAdapter.class},
                (proxy, method, arguments) -> switch (method.getName()) {
                    case "createDealerSocket" -> dealer;
                    default -> throw new UnsupportedOperationException(
                        method.getName());
                });
        ZLinkBackendAdapterProvider provider =
            (ZLinkBackendAdapterProvider) Proxy.newProxyInstance(
                ZLinkBackendAdapterProvider.class.getClassLoader(),
                new Class<?>[] {ZLinkBackendAdapterProvider.class},
                (proxy, method, arguments) -> switch (method.getName()) {
                    case "createChannelAdapter" -> adapter;
                    case "createMonitoringAdapter" ->
                        immediateReadyMonitoringAdapter();
                    default -> throw new UnsupportedOperationException(
                        method.getName());
                });
        ZLinkLocationRepository store =
            new SingleDescriptorStore(descriptor);
        ZLinkBackendContext context =
            (ZLinkBackendContext) Proxy.newProxyInstance(
                ZLinkBackendContext.class.getClassLoader(),
                new Class<?>[] {ZLinkBackendContext.class},
                (proxy, method, arguments) -> switch (method.getName()) {
                    case "name" -> "context";
                    case "close", "shutdown" -> null;
                    default -> throw new UnsupportedOperationException(
                        method.getName());
                });
        var scheduler = Executors.newSingleThreadScheduledExecutor();
        var infrastructure = Executors.newVirtualThreadPerTaskExecutor();
        ZLinkClientServerLocationRuntime runtime =
            new ZLinkClientServerLocationRuntime(
                store,
                () -> new ZLinkLocationOwnerToken("owner", 1),
                provider,
                context,
                new ZLinkBackendAdapterOptions(Duration.ofSeconds(1)),
                sockets,
                scheduler,
                infrastructure,
                Duration.ofHours(1),
                100);
        ZLinkChannelRuntime.AutoConnectSurface client =
            new ZLinkChannelRuntime.AutoConnectSurface(
                    systems.zlink.framework.runtime.internal.locations
                        .ZLinkAutoConnectType.CLIENT_SERVER,
                "orders",
                ZLinkLocationRole.DEALER,
                RoutingId.from("client"),
                "",
                100,
                null,
                List.of());
        ZLinkChannelRuntime.AutoConnectSurface server =
            new ZLinkChannelRuntime.AutoConnectSurface(
                    systems.zlink.framework.runtime.internal.locations
                        .ZLinkAutoConnectType.CLIENT_SERVER,
                "orders",
                ZLinkLocationRole.ROUTER,
                descriptor.serverRid(),
                descriptor.endpoint(),
                descriptor.weight(),
                null,
                List.of());

        try {
            runtime.start(List.of(client, server)).toCompletableFuture().join();

            assertSame(
                dealer,
                sockets.awaitClientForOutbound(
                    "orders", Duration.ofSeconds(1)));
            runtime.stop().toCompletableFuture().join();
        } finally {
            runtime.close();
            scheduler.shutdownNow();
            infrastructure.shutdownNow();
        }
    }

    @Test
    void stopWaitsForAdmittedPublicationBeforeRemovingDescriptor()
        throws Exception {
        BlockingUpdateStore store = new BlockingUpdateStore();
        ZLinkChannelSocketRegistry sockets = new ZLinkChannelSocketRegistry();
        ZLinkChannelBackendAdapter adapter =
            (ZLinkChannelBackendAdapter) Proxy.newProxyInstance(
                ZLinkChannelBackendAdapter.class.getClassLoader(),
                new Class<?>[] {ZLinkChannelBackendAdapter.class},
                (proxy, method, arguments) -> switch (method.getName()) {
                    case "name" -> "location-test";
                    default -> throw new UnsupportedOperationException(
                        method.getName());
                });
        ZLinkBackendAdapterProvider provider =
            (ZLinkBackendAdapterProvider) Proxy.newProxyInstance(
                ZLinkBackendAdapterProvider.class.getClassLoader(),
                new Class<?>[] {ZLinkBackendAdapterProvider.class},
                (proxy, method, arguments) -> switch (method.getName()) {
                    case "createChannelAdapter" -> adapter;
                    default -> throw new UnsupportedOperationException(
                        method.getName());
                });
        ZLinkBackendContext context =
            (ZLinkBackendContext) Proxy.newProxyInstance(
                ZLinkBackendContext.class.getClassLoader(),
                new Class<?>[] {ZLinkBackendContext.class},
                (proxy, method, arguments) -> switch (method.getName()) {
                    case "name" -> "context";
                    case "close", "shutdown" -> null;
                    default -> throw new UnsupportedOperationException(
                        method.getName());
                });
        var scheduler = Executors.newSingleThreadScheduledExecutor();
        var infrastructure = Executors.newVirtualThreadPerTaskExecutor();
        ZLinkClientServerLocationRuntime runtime =
            new ZLinkClientServerLocationRuntime(
                store,
                () -> new ZLinkLocationOwnerToken("owner", 1),
                provider,
                context,
                new ZLinkBackendAdapterOptions(Duration.ofSeconds(1)),
                sockets,
                scheduler,
                infrastructure,
                Duration.ofHours(1),
                100);
        ZLinkChannelRuntime.AutoConnectSurface server =
            new ZLinkChannelRuntime.AutoConnectSurface(
                systems.zlink.framework.runtime.internal.locations
                    .ZLinkAutoConnectType.CLIENT_SERVER,
                "orders",
                ZLinkLocationRole.ROUTER,
                RoutingId.from("server"),
                "tcp://127.0.0.1:7010",
                100,
                null,
                List.of());

        try {
            CompletionStage<Void> started = runtime.start(List.of(server));
            store.updateAdmitted.get(1, TimeUnit.SECONDS);
            assertTrue(store.hasCapturedUpdate());

            CompletionStage<Void> stopped = runtime.stop();
            assertFalse(stopped.toCompletableFuture().isDone());
            assertEquals(List.of(), store.operations());

            store.releaseUpdate();
            started.toCompletableFuture().get(1, TimeUnit.SECONDS);
            stopped.toCompletableFuture().get(1, TimeUnit.SECONDS);

            assertEquals(List.of("update", "remove"), store.operations());
            assertNull(store.current());
        } finally {
            runtime.close();
            scheduler.shutdownNow();
            infrastructure.shutdownNow();
        }
    }

    private static ZLinkClientServerServerDescriptor descriptor(
        String channelName,
        RoutingId rid,
        long lifecycle,
        long revision,
        String endpoint,
        int weight) {
        return new ZLinkClientServerServerDescriptor(
            channelName,
            rid,
            lifecycle,
            revision,
            endpoint,
            weight,
            ZLinkFrameworkRuntimeState.SERVING,
            "default",
            "owner",
            1,
            Instant.EPOCH);
    }

    private static ZLinkBackendDealerSocket dealer(String name) {
        return (ZLinkBackendDealerSocket) Proxy.newProxyInstance(
            ZLinkBackendDealerSocket.class.getClassLoader(),
            new Class<?>[] {ZLinkBackendDealerSocket.class},
            (proxy, method, arguments) -> switch (method.getName()) {
                case "name" -> name;
                case "equals" -> proxy == arguments[0];
                case "hashCode" -> System.identityHashCode(proxy);
                case "close", "connect", "disconnect", "bind",
                    "setChannelName" -> null;
                case "send" -> CompletableFuture.completedFuture(null);
                case "request" -> new CompletableFuture<ZLinkBackendReceived>();
                case "recv" -> null;
                default -> throw new UnsupportedOperationException(
                    method.getName());
            });
    }

    private static ZLinkBackendDealerSocket admittingDealer(
        String name,
        ZLinkClientServerServerDescriptor descriptor) {
        return (ZLinkBackendDealerSocket) Proxy.newProxyInstance(
            ZLinkBackendDealerSocket.class.getClassLoader(),
            new Class<?>[] {ZLinkBackendDealerSocket.class},
            (proxy, method, arguments) -> switch (method.getName()) {
                case "name" -> name;
                case "equals" -> proxy == arguments[0];
                case "hashCode" -> System.identityHashCode(proxy);
                case "close", "connect", "disconnect", "bind",
                    "setChannelName" -> null;
                case "request" -> {
                    Message response = Message.from(
                        ZLinkClientServerServiceWire.encodeAdmit(
                            descriptor, Integer.MAX_VALUE));
                    yield CompletableFuture.completedFuture(
                        new ZLinkBackendReceived(
                        ZLinkBackendRequestResult.OK,
                        Optional.empty(),
                        Optional.empty(),
                        Optional.empty(),
                        List.of(response)));
                }
                case "send" -> CompletableFuture.completedFuture(null);
                case "recv" -> null;
                default -> throw new UnsupportedOperationException(
                    method.getName());
            });
    }

    private static final class SingleDescriptorStore
        extends ZLinkLocationStoreTestAdapter {
        private final ZLinkClientServerServerDescriptor descriptor;

        private SingleDescriptorStore(
            ZLinkClientServerServerDescriptor descriptor) {
            this.descriptor = descriptor;
        }

        @Override
        public CompletionStage<ZLinkLocationWriteResult>
            updateClientServer(
                ZLinkClientServerServerDescriptor value,
                ZLinkLocationWriteIntent intent) {
            return CompletableFuture.completedFuture(
                ZLinkLocationWriteResult.stored(
                    value.leaseGeneration(), Instant.now()));
        }

        @Override
        public CompletionStage<ZLinkLocationWriteStatus>
            removeClientServer(
                ZLinkClientServerServerDescriptorKey key,
                ZLinkLocationOwnerToken owner) {
            return CompletableFuture.completedFuture(
                ZLinkLocationWriteStatus.STORED);
        }

        @Override
        public CompletionStage<
            ZLinkLocationPage<ZLinkClientServerServerDescriptor>>
            listClientServers(
                String channelName,
                ZLinkPageRequest page) {
            return CompletableFuture.completedFuture(
                new ZLinkLocationPage<>(List.of(descriptor), null));
        }
    }

    private static final class BlockingUpdateStore
        extends ZLinkLocationStoreTestAdapter {
        private final CompletableFuture<Void> updateAdmitted =
            new CompletableFuture<>();
        private final CompletableFuture<ZLinkLocationWriteResult> update =
            new CompletableFuture<>();
        private final List<String> operations = new ArrayList<>();
        private ZLinkClientServerServerDescriptor captured;
        private ZLinkClientServerServerDescriptor current;

        @Override
        public synchronized CompletionStage<ZLinkLocationWriteResult>
            updateClientServer(
                ZLinkClientServerServerDescriptor descriptor,
                ZLinkLocationWriteIntent intent) {
            captured = descriptor;
            updateAdmitted.complete(null);
            return update;
        }

        synchronized boolean hasCapturedUpdate() {
            return captured != null;
        }

        synchronized void releaseUpdate() {
            current = captured;
            operations.add("update");
            update.complete(ZLinkLocationWriteResult.stored(
                captured.leaseGeneration(), Instant.now()));
        }

        @Override
        public synchronized CompletionStage<ZLinkLocationWriteStatus>
            removeClientServer(
                ZLinkClientServerServerDescriptorKey key,
                ZLinkLocationOwnerToken owner) {
            current = null;
            operations.add("remove");
            return CompletableFuture.completedFuture(
                ZLinkLocationWriteStatus.STORED);
        }

        @Override
        public CompletionStage<
            ZLinkLocationPage<ZLinkClientServerServerDescriptor>>
            listClientServers(String channelName, ZLinkPageRequest page) {
            return CompletableFuture.completedFuture(
                new ZLinkLocationPage<>(List.of(), null));
        }

        synchronized List<String> operations() {
            return List.copyOf(operations);
        }

        synchronized ZLinkClientServerServerDescriptor current() {
            return current;
        }
    }

    private static ZLinkBackendRouterSocket router() {
        return (ZLinkBackendRouterSocket) Proxy.newProxyInstance(
            ZLinkBackendRouterSocket.class.getClassLoader(),
            new Class<?>[] {ZLinkBackendRouterSocket.class},
            (proxy, method, arguments) -> switch (method.getName()) {
                case "name" -> "router";
                case "maxMessageSize" -> 4096L;
                case "peerWeight" -> 100;
                case "lastEndpoint" -> "tcp://127.0.0.1:7001";
                case "close", "connect", "disconnect", "bind",
                    "setChannelName", "setRoutingId",
                    "setConnectRoutingId", "setProbe",
                    "setMaxMessageSize", "setPeerWeight", "reply" -> null;
                case "send", "request" -> false;
                case "recv" -> null;
                default -> throw new UnsupportedOperationException(
                    method.getName());
            });
    }

    private static ZLinkMonitoringBackendAdapter
        immediateReadyMonitoringAdapter() {
        java.util.concurrent.atomic.AtomicBoolean delivered =
            new java.util.concurrent.atomic.AtomicBoolean();
        return socket -> (ZLinkBackendSocketMonitor)
            Proxy.newProxyInstance(
                ZLinkBackendSocketMonitor.class.getClassLoader(),
                new Class<?>[] {ZLinkBackendSocketMonitor.class},
                (proxy, method, arguments) -> switch (method.getName()) {
                    case "name" -> "monitor";
                    case "recv" -> delivered.compareAndSet(false, true)
                        ? new ZLinkBackendSocketMonitorEvent(
                            "CONNECTION_READY", Optional.empty(), "", "")
                        : null;
                    case "close" -> null;
                    default -> throw new UnsupportedOperationException(
                        method.getName());
                });
    }

    private static ZLinkBackendReceived received(byte[] frame) {
        return new ZLinkBackendReceived(
            Optional.empty(),
            Optional.empty(),
            Optional.empty(),
            List.of(Message.from(frame)));
    }

    private static final class ControlledDealer
        implements ZLinkBackendDealerSocket {
        private final Deque<ZLinkBackendReceived> inbound =
            new ArrayDeque<>();
        private final List<byte[]> sent = new ArrayList<>();
        private final List<byte[]> requests = new ArrayList<>();

        @Override public void setReceiveFlowState(
            systems.zlink.contracts.sockets.ReceiveFlowState state) { }
        @Override public String name() {
            return "controlled";
        }

        @Override public void setChannelName(String channelName) {
        }

        @Override public void bind(String endpoint) {
        }

        @Override public void connect(String endpoint) {
        }

        @Override public void disconnect(String endpoint) {
        }

        @Override public CompletionStage<Void> send(List<Message> parts) {
            sent.add(parts.get(0).toByteArray());
            return CompletableFuture.completedFuture(null);
        }

        @Override public CompletionStage<ZLinkBackendReceived> request(
            List<Message> parts,
            Duration timeout) {
            requests.add(parts.get(0).toByteArray());
            return new CompletableFuture<>();
        }

        @Override public ZLinkBackendReceived recv(
            ZLinkBackendRecvMode mode) {
            return inbound.pollFirst();
        }

        @Override public boolean waitForReadable(Duration timeout) {
            return !inbound.isEmpty();
        }

        @Override public void close() {
            while (!inbound.isEmpty()) {
                inbound.removeFirst().close();
            }
        }
    }

    private static final class ControlledRouter
        implements ZLinkBackendRouterSocket {
        private final List<byte[]> sent = new ArrayList<>();
        private final List<RoutingId> disconnected =
            new ArrayList<>();
        private boolean acceptSend = true;
        private RuntimeException sendFailure;

        @Override public void setReceiveFlowState(
            systems.zlink.contracts.sockets.ReceiveFlowState state) { }
        @Override public String name() {
            return "controlled-router";
        }
        @Override public void bind(String endpoint) {
        }
        @Override public void connect(String endpoint) {
        }
        @Override public void disconnect(String endpoint) {
        }
        @Override public void setChannelName(String channelName) {
        }
        @Override public void setRoutingId(RoutingId routingId) {
        }
        @Override public void setConnectRoutingId(RoutingId routingId) {
        }
        @Override public void setProbe(boolean enabled) {
        }
        @Override public long maxMessageSize() {
            return 4096;
        }
        @Override public void setMaxMessageSize(long value) {
        }
        @Override public int peerWeight() {
            return 100;
        }
        @Override public void setPeerWeight(int weight) {
        }
        @Override public ZLinkBackendReceived recv(
            ZLinkBackendRecvMode mode) {
            return null;
        }
        @Override public boolean waitForReadable(Duration timeout) {
            return false;
        }
        @Override public CompletionStage<Void> send(
            RoutingId routingId,
            List<Message> parts) {
            if (sendFailure != null) {
                return CompletableFuture.failedFuture(sendFailure);
            }
            sent.add(parts.get(0).toByteArray());
            return acceptSend
                ? CompletableFuture.completedFuture(null)
                : CompletableFuture.failedFuture(
                    new ZlinkSubmitException(SubmitResult.NOT_ADMITTED));
        }
        @Override public CompletionStage<ZLinkBackendReceived> request(
            RoutingId routingId,
            List<Message> parts,
            Duration timeout) {
            return new CompletableFuture<>();
        }
        @Override public void reply(
            RoutingId routingId,
            long requestSeq,
            List<Message> parts) {
        }
        @Override public void disconnectPeer(RoutingId routingId) {
            disconnected.add(routingId);
        }
        @Override public void close() {
        }
    }
}
