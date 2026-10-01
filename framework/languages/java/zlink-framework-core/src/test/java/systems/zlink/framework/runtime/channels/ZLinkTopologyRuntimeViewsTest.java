package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.monitoring.ZLinkPeerState;
import systems.zlink.framework.monitoring.ZLinkTopologyReason;
import systems.zlink.framework.monitoring.ZLinkTopologyState;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;

import java.lang.reflect.Proxy;
import java.time.Instant;
import java.util.List;
import java.util.concurrent.Flow;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkTopologyRuntimeViewsTest {
    @Test
    void observersShareOneChangedStatusSequenceWithoutQueryGaps() throws Exception {
        var sockets = new ZLinkChannelSocketRegistry();
        var channel = new ChannelRegistration("events", ChannelKind.FANOUT);
        channel.enableSubscriber();
        sockets.registerChannel(channel);
        var hostState = new AtomicReference<>(ZLinkFrameworkRuntimeState.SERVING);
        var view = new ZLinkFanoutRuntimeView(sockets, () -> null, () -> null, hostState::get);
        try (var first =
                        new StatusCollector<
                                systems.zlink.framework.monitoring.ZLinkFanoutStatus>();
                var second =
                        new StatusCollector<
                                systems.zlink.framework.monitoring.ZLinkFanoutStatus>()) {
            view.observe("events", 1).subscribe(first);
            view.observe("events", 8).subscribe(second);
            assertEquals(1, first.next().sequence());
            assertEquals(1, second.next().sequence());
            view.snapshot("events");
            view.snapshot("events");
            hostState.set(ZLinkFrameworkRuntimeState.DRAINING);
            sockets.signalTopologyChanged();
            var firstChanged = first.next();
            var secondChanged = second.next();
            assertEquals(2, firstChanged.sequence());
            assertEquals(firstChanged, secondChanged);
            assertEquals(2, view.snapshot("events").sequence());
        }
    }

    private static final class StatusCollector<T>
            implements Flow.Subscriber<ZLinkObservedStatus<T>>, AutoCloseable {
        private final LinkedBlockingQueue<T> received = new LinkedBlockingQueue<>();
        private Flow.Subscription subscription;

        @Override
        public void onSubscribe(Flow.Subscription value) {
            subscription = value;
            value.request(Long.MAX_VALUE);
        }

        @Override
        public void onNext(ZLinkObservedStatus<T> value) {
            received.add(value.status());
        }

        @Override
        public void onError(Throwable failure) {
            throw new AssertionError(failure);
        }

        @Override
        public void onComplete() {}

        private T next() throws Exception {
            T status = received.poll(1, TimeUnit.SECONDS);
            org.junit.jupiter.api.Assertions.assertNotNull(status);
            return status;
        }

        @Override
        public void close() {
            subscription.cancel();
        }
    }

    @Test
    void terminalChannelSourceCannotRegressOnQueryOrLateObservation() throws Exception {
        var sockets = new ZLinkChannelSocketRegistry();
        var channel = new ChannelRegistration("events", ChannelKind.FANOUT);
        channel.enableSubscriber();
        sockets.registerChannel(channel);
        var hostState = new AtomicReference<>(ZLinkFrameworkRuntimeState.SERVING);
        var view = new ZLinkFanoutRuntimeView(sockets, () -> null, () -> null, hostState::get);
        var first = firstObserved(view.observe("events", 8));
        hostState.set(ZLinkFrameworkRuntimeState.STOPPED);
        sockets.signalTopologyChanged();
        var stopped = firstObserved(view.observe("events", 8));
        assertEquals(ZLinkTopologyState.STOPPED, stopped.state());
        assertEquals(first.sequence() + 1, stopped.sequence());
        hostState.set(ZLinkFrameworkRuntimeState.SERVING);
        sockets.signalTopologyChanged();
        assertEquals(stopped, view.snapshot("events"));
        assertEquals(stopped, firstObserved(view.observe("events", 8)));
    }

    @Test
    void channelSourcesShareSequenceAcrossQueriesAndObservers() throws Exception {
        var sockets = new ZLinkChannelSocketRegistry();
        for (String name : List.of("orders", "other-orders")) {
            var channel = new ChannelRegistration(name, ChannelKind.CLIENT_SERVER);
            channel.enableClient();
            sockets.registerChannel(channel);
        }
        for (String name : List.of("events", "other-events")) {
            var channel = new ChannelRegistration(name, ChannelKind.FANOUT);
            channel.enableSubscriber();
            sockets.registerChannel(channel);
        }
        var clients =
                new ZLinkClientServerRuntimeView(sockets, () -> ZLinkFrameworkRuntimeState.SERVING);
        var fanout =
                new ZLinkFanoutRuntimeView(
                        sockets, () -> null, () -> null, () -> ZLinkFrameworkRuntimeState.SERVING);
        long clientsSequence = clients.snapshot("orders").sequence();
        long fanoutSequence = fanout.snapshot("events").sequence();
        assertEquals(1, clientsSequence);
        assertEquals(1, fanoutSequence);
        clients.snapshot("other-orders");
        fanout.snapshot("other-events");
        assertEquals(clientsSequence, clients.snapshot("orders").sequence());
        assertEquals(fanoutSequence, fanout.snapshot("events").sequence());
        assertEquals(clientsSequence, firstObserved(clients.observe("orders", 8)).sequence());
        assertEquals(clientsSequence, firstObserved(clients.observe("orders", 8)).sequence());
        assertEquals(fanoutSequence, firstObserved(fanout.observe("events", 8)).sequence());
        assertEquals(fanoutSequence, firstObserved(fanout.observe("events", 8)).sequence());
    }

    private static <T> T firstObserved(Flow.Publisher<ZLinkObservedStatus<T>> publisher)
            throws Exception {
        var received = new LinkedBlockingQueue<T>();
        publisher.subscribe(
                new Flow.Subscriber<>() {
                    private Flow.Subscription subscription;

                    @Override
                    public void onSubscribe(Flow.Subscription value) {
                        subscription = value;
                        value.request(1);
                    }

                    @Override
                    public void onNext(ZLinkObservedStatus<T> value) {
                        received.add(value.status());
                        subscription.cancel();
                    }

                    @Override
                    public void onError(Throwable failure) {
                        throw new AssertionError(failure);
                    }

                    @Override
                    public void onComplete() {}
                });
        T status = received.poll(1, TimeUnit.SECONDS);
        org.junit.jupiter.api.Assertions.assertNotNull(status);
        return status;
    }

    @Test
    void clientServerConnectionAwaitingAdmissionIsConnecting() {
        var sockets = new ZLinkChannelSocketRegistry();
        var channel = new ChannelRegistration("orders", ChannelKind.CLIENT_SERVER);
        channel.enableClient();
        sockets.registerChannel(channel);
        var dealer =
                (ZLinkBackendDealerSocket)
                        Proxy.newProxyInstance(
                                ZLinkBackendDealerSocket.class.getClassLoader(),
                                new Class<?>[] {ZLinkBackendDealerSocket.class},
                                (proxy, method, arguments) -> {
                                    throw new UnsupportedOperationException(method.getName());
                                });
        sockets.addClientServerConnection(
                "remote",
                new ZLinkClientServerServerDescriptor(
                        "orders",
                        RoutingId.from("remote-target"),
                        1,
                        1,
                        "tcp://127.0.0.1:7001",
                        100,
                        ZLinkFrameworkRuntimeState.SERVING,
                        "default",
                        "remote-owner",
                        1,
                        Instant.EPOCH),
                dealer);
        var view =
                new ZLinkClientServerRuntimeView(sockets, () -> ZLinkFrameworkRuntimeState.SERVING);
        var target = view.snapshot("orders").targets().getFirst();
        assertEquals(ZLinkPeerState.CONNECTING, target.state());
        assertEquals(ZLinkTopologyReason.NO_READY_TARGET, target.unavailableReason().orElseThrow());
    }

    @Test
    void clientServerTargetProjectsDescriptorLifecycleWithoutChangingWeightMeaning() {
        var sockets = new ZLinkChannelSocketRegistry();
        var channel = new ChannelRegistration("orders", ChannelKind.CLIENT_SERVER);
        channel.enableServer();
        sockets.registerChannel(channel);
        var runtime =
                new ZLinkClientServerRuntimeView(sockets, () -> ZLinkFrameworkRuntimeState.SERVING);
        for (var state : ZLinkFrameworkRuntimeState.values()) {
            sockets.setClientServerServerDescriptor(
                    "orders",
                    new ZLinkClientServerServerDescriptor(
                            "orders",
                            RoutingId.from("lifecycle-target"),
                            1,
                            1,
                            "tcp://127.0.0.1:7001",
                            0,
                            state,
                            "default",
                            "local-owner",
                            1,
                            Instant.EPOCH));
            var target = runtime.snapshot("orders").targets().getFirst();
            var expectedState =
                    switch (state) {
                        case RELOCATING, RELOCATED, DRAINING -> ZLinkPeerState.DRAINING;
                        case SERVING -> ZLinkPeerState.READY;
                        default -> ZLinkPeerState.NOT_CONNECTED;
                    };
            assertEquals(expectedState, target.state(), state.name());
            assertEquals(0, target.weight());
            if (state == ZLinkFrameworkRuntimeState.SERVING) {
                assertTrue(target.unavailableReason().isEmpty());
            } else {
                assertEquals(
                        expectedState == ZLinkPeerState.DRAINING
                                ? ZLinkTopologyReason.DRAINING
                                : ZLinkTopologyReason.NO_READY_TARGET,
                        target.unavailableReason().orElseThrow(),
                        state.name());
            }
        }
    }

    @Test
    void hostRelocationMakesClientServerAndFanoutNotReady() {
        ZLinkChannelSocketRegistry sockets = new ZLinkChannelSocketRegistry();
        ChannelRegistration clientServer =
                new ChannelRegistration("orders", ChannelKind.CLIENT_SERVER);
        clientServer.enableServer();
        sockets.registerChannel(clientServer);
        RoutingId serverRid = RoutingId.from("local-server");
        var router =
                (ZLinkBackendRouterSocket)
                        Proxy.newProxyInstance(
                                ZLinkBackendRouterSocket.class.getClassLoader(),
                                new Class<?>[] {ZLinkBackendRouterSocket.class},
                                (proxy, method, arguments) -> {
                                    if (method.getName().equals("peerWeight")) {
                                        return 100;
                                    }
                                    throw new UnsupportedOperationException(method.getName());
                                });
        sockets.registerServer("orders", serverRid, router);
        sockets.setClientServerServerDescriptor(
                "orders",
                new ZLinkClientServerServerDescriptor(
                        "orders",
                        serverRid,
                        1,
                        1,
                        "tcp://127.0.0.1:7001",
                        100,
                        ZLinkFrameworkRuntimeState.SERVING,
                        "default",
                        "local-owner",
                        1,
                        Instant.EPOCH));
        ChannelRegistration fanout = new ChannelRegistration("events", ChannelKind.FANOUT);
        fanout.enableSubscriber();
        sockets.registerChannel(fanout);
        AtomicReference<ZLinkFrameworkRuntimeState> hostState =
                new AtomicReference<>(ZLinkFrameworkRuntimeState.SERVING);
        var clientServerRuntime = new ZLinkClientServerRuntimeView(sockets, hostState::get);
        var fanoutRuntime =
                new ZLinkFanoutRuntimeView(sockets, () -> null, () -> null, hostState::get);

        assertTrue(clientServerRuntime.snapshot("orders").isReady());
        assertEquals(1, clientServerRuntime.snapshot("orders").readyTargetCount());

        hostState.set(ZLinkFrameworkRuntimeState.RELOCATING);

        assertFalse(clientServerRuntime.snapshot("orders").isReady());
        assertEquals(1, clientServerRuntime.snapshot("orders").readyTargetCount());
        assertEquals(ZLinkTopologyState.STOPPING, clientServerRuntime.snapshot("orders").state());
        assertFalse(fanoutRuntime.snapshot("events").isReady());
        assertEquals(ZLinkTopologyState.STOPPING, fanoutRuntime.snapshot("events").state());
    }

    @Test
    void stoppedAndErrorHostStatesAreProjectedExactly() {
        ZLinkChannelSocketRegistry sockets = new ZLinkChannelSocketRegistry();
        ChannelRegistration clientServer =
                new ChannelRegistration("orders", ChannelKind.CLIENT_SERVER);
        clientServer.enableServer();
        sockets.registerChannel(clientServer);
        ChannelRegistration fanout = new ChannelRegistration("events", ChannelKind.FANOUT);
        fanout.enableSubscriber();
        sockets.registerChannel(fanout);
        AtomicReference<ZLinkFrameworkRuntimeState> hostState =
                new AtomicReference<>(ZLinkFrameworkRuntimeState.STOPPED);
        var clientServerRuntime = new ZLinkClientServerRuntimeView(sockets, hostState::get);
        var fanoutRuntime =
                new ZLinkFanoutRuntimeView(sockets, () -> null, () -> null, hostState::get);

        assertEquals(ZLinkTopologyState.STOPPED, clientServerRuntime.snapshot("orders").state());
        assertEquals(ZLinkTopologyState.STOPPED, fanoutRuntime.snapshot("events").state());

        var failedClients =
                new ZLinkClientServerRuntimeView(sockets, () -> ZLinkFrameworkRuntimeState.ERROR);
        var failedFanout =
                new ZLinkFanoutRuntimeView(
                        sockets, () -> null, () -> null, () -> ZLinkFrameworkRuntimeState.ERROR);
        assertEquals(ZLinkTopologyState.FAILED, failedClients.snapshot("orders").state());
        assertEquals(ZLinkTopologyState.FAILED, failedFanout.snapshot("events").state());
    }
}
