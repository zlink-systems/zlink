package systems.zlink.samples.kotlin.zoneworld.server.zone;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.locks.LockSupport;
import org.junit.jupiter.api.Test;
import org.springframework.context.SmartLifecycle;
import org.springframework.context.support.GenericApplicationContext;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.spots.ZLinkSpotRequestCall;
import systems.zlink.framework.spots.ZLinkSpotSendCall;
import systems.zlink.samples.kotlin.zoneworld.server.configuration.NodeCensus;
import systems.zlink.samples.kotlin.zoneworld.server.configuration.NodeMaintenanceState;
import systems.zlink.samples.kotlin.zoneworld.server.configuration.SampleTopology;

final class ZoneStatusReporterLifecycleTest {
    @Test
    void runtimePortProducerStopsBeforeRuntimeLifecycle() throws Exception {
        RuntimePort runtime = new RuntimePort();
        ZoneStatusReporter reporter = reporter(runtime);
        runtime.producer = reporter;
        GenericApplicationContext context = new GenericApplicationContext();
        context.registerBean("runtimePort", RuntimePort.class, () -> runtime);
        context.registerBean("zoneStatusReporter", ZoneStatusReporter.class, () -> reporter);
        try {
            context.refresh();
            runtime.firstSubmission.get(2, TimeUnit.SECONDS);

            context.close();

            assertEquals(0, runtime.postStopSubmissions.get(),
                "the status producer must stop before its route runtime port");
            assertEquals(0, runtime.runningProducerStops.get(),
                "the route runtime must not observe a running status producer while stopping");
        } finally {
            context.close();
        }
    }

    @Test
    void smartLifecycleStopCompletesItsCallbackExactlyOnce() throws Exception {
        RuntimePort runtime = new RuntimePort();
        ZoneStatusReporter reporter = reporter(runtime);
        AtomicInteger callbacks = new AtomicInteger();
        reporter.start();
        runtime.firstSubmission.get(2, TimeUnit.SECONDS);

        reporter.stop(callbacks::incrementAndGet);

        assertEquals(1, callbacks.get());
        assertFalse(reporter.isRunning());
    }

    private static ZoneStatusReporter reporter(RuntimePort runtime) {
        return new ZoneStatusReporter(
            new SampleTopology(
                "zone",
                "zone-node-1",
                "tcp://127.0.0.1:1",
                null,
                "redis://127.0.0.1:1",
                "test:"),
            runtime,
            new NodeCensus(),
            new NodeMaintenanceState());
    }

    private static final class RuntimePort implements ZLinkRouteClient, SmartLifecycle {
        private final CompletableFuture<Void> firstSubmission = new CompletableFuture<>();
        private final AtomicInteger postStopSubmissions = new AtomicInteger();
        private final AtomicInteger runningProducerStops = new AtomicInteger();
        private ZoneStatusReporter producer;
        private volatile boolean running;
        private volatile boolean stopped;

        @Override
        public ZLinkSendCall sendToChannel(String channelName, Object message) {
            return () -> {
                if (stopped) {
                    postStopSubmissions.incrementAndGet();
                }
                firstSubmission.complete(null);
                return CompletableFuture.completedFuture(null);
            };
        }

        @Override
        public ZLinkRequestCall requestToChannel(String channelName, Object request) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkSendCall sendToNode(String channelName, RoutingId target, Object message) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkSpotSendCall sendToSpot(String spotId, Object message) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkRequestCall requestToNode(String channelName, RoutingId target, Object message) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkSpotRequestCall requestToSpot(String spotId, Object message) {
            throw new UnsupportedOperationException();
        }

        @Override
        public void start() {
            running = true;
        }

        @Override
        public void stop() {
            running = false;
            stopped = true;
            Object currentProducer = producer;
            if (!(currentProducer instanceof SmartLifecycle lifecycle)
                || lifecycle.isRunning()) {
                runningProducerStops.incrementAndGet();
            }
            LockSupport.parkNanos(TimeUnit.MILLISECONDS.toNanos(2_200));
        }

        @Override
        public boolean isRunning() {
            return running;
        }

        @Override
        public int getPhase() {
            return 0;
        }
    }
}
