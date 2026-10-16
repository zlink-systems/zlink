package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.handlers.ZLinkSpotSubscription;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateState;
import systems.zlink.framework.spots.ZLinkSpotSubscriptionHandler;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.logging.FileHandler;
import java.util.logging.Handler;
import java.util.logging.Level;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class ZLinkRemoteMulticastPermitTest {
    private static final int SUBSCRIBERS = 3;
    private static final int TIMEOUT_SECONDS = 5;
    private static volatile CountDownLatch delivered = new CountDownLatch(0);

    @Test
    void remoteMulticastReachesEverySubscriberOnAHostWithOnePermit() throws Exception {
        var flowLogger =
                Logger.getLogger(
                        "systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer");
        var previousLevel = flowLogger.getLevel();
        var flowLog = new FileHandler("build/remote-multicast-permit.flow");
        flowLogger.addHandler(flowLog);
        flowLogger.setLevel(Level.ALL);
        try {
            // Spec 04 application job queue: the receive permit of the multicast record admits one
            // child and each further child waits for its own permit. The receive path must not hold
            // its permit while it waits for another one, or a host with one permit never delivers.
            delivered = new CountDownLatch(SUBSCRIBERS);
            PublisherSpot.createdContext.set(null);
            String suffix = Long.toUnsignedString(System.nanoTime(), 36);
            RoutingId targetRid = RoutingId.from("multicast-permit-target-" + suffix);
            var store = new ZLinkInMemoryLocationStore();

            var targetOptions = new DefaultZLinkFrameworkOptions();
            targetOptions.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            targetOptions.addLocationStore(store);
            targetOptions.registration().inboundDispatch().setMaxQueuedApplicationJobs(1);
            var targetNode = targetOptions.addRouteMesh("game");
            targetNode.listen("tcp://127.0.0.1:0").setRoutingId(targetRid);
            targetNode.channelName("events").server().setWeight(10_000);
            targetNode
                    .objects()
                    .server()
                    .addSpotFactory(
                            "Subscriber",
                            SubscriberSpot.class,
                            factory -> factory.disableRelocation());

            try (ZLinkFrameworkRuntime target =
                    ZLinkFrameworkRuntimeTestAccess.start(
                            targetOptions, new ZLinkJavaBackendAdapterFactory())) {
                for (int index = 0; index < SUBSCRIBERS; index++) {
                    var created =
                            target.spotManager()
                                    .create("Subscriber")
                                    .submit()
                                    .toCompletableFuture()
                                    .get(TIMEOUT_SECONDS, TimeUnit.SECONDS);
                    assertEquals(ZLinkSpotCreateState.CREATED, created.state());
                }
                String targetEndpoint =
                        target.listenerStatus(ZLinkListenerKind.ROUTE_MESH, "game").endpoint();

                var sourceOptions = new DefaultZLinkFrameworkOptions();
                sourceOptions.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
                // Explicit peering supplies the remote topology. Separate stores keep discovery
                // from concurrently creating a second peer connection in this permit test.
                sourceOptions.addLocationStore(new ZLinkInMemoryLocationStore());
                var sourceNode = sourceOptions.addRouteMesh("game");
                sourceNode
                        .listen("tcp://127.0.0.1:0")
                        .setRoutingId(RoutingId.from("multicast-permit-source-" + suffix));
                sourceNode.channelName("events").server().setWeight(10_000);
                sourceNode.peerConnections().connect(targetEndpoint);
                sourceNode
                        .objects()
                        .server()
                        .addSpotFactory(
                                "Publisher",
                                PublisherSpot.class,
                                factory -> factory.disableRelocation());

                // The source logs this line once the target is a ready peer it multicasts to.
                var targetReady = new CountDownLatch(1);
                String readyLine = "ZLINK_FRAMEWORK_PEER_READY mesh=game peer=" + targetRid;
                var meshLogger =
                        Logger.getLogger(
                                "systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNode");
                var readyHandler =
                        new Handler() {
                            @Override
                            public void publish(LogRecord record) {
                                if (readyLine.equals(record.getMessage())) targetReady.countDown();
                            }

                            @Override
                            public void flush() {}

                            @Override
                            public void close() {}
                        };
                meshLogger.addHandler(readyHandler);
                try (ZLinkFrameworkRuntime source =
                        ZLinkFrameworkRuntimeTestAccess.start(
                                sourceOptions, new ZLinkJavaBackendAdapterFactory())) {
                    assertTrue(
                            targetReady.await(TIMEOUT_SECONDS, TimeUnit.SECONDS),
                            "target peer never ready");
                    var publisher =
                            source.spotManager()
                                    .create("Publisher")
                                    .submit()
                                    .toCompletableFuture()
                                    .get(TIMEOUT_SECONDS, TimeUnit.SECONDS);
                    assertEquals(ZLinkSpotCreateState.CREATED, publisher.state());
                    PublisherSpot.createdContext
                            .get()
                            .outbound()
                            .publish("events", "orders", "probe")
                            .submit()
                            .toCompletableFuture()
                            .get(TIMEOUT_SECONDS, TimeUnit.SECONDS);

                    assertTrue(
                            delivered.await(TIMEOUT_SECONDS, TimeUnit.SECONDS),
                            "subscribers that received the multicast: "
                                    + (SUBSCRIBERS - delivered.getCount())
                                    + " of "
                                    + SUBSCRIBERS);
                } finally {
                    meshLogger.removeHandler(readyHandler);
                }
            }
        } finally {
            flowLogger.removeHandler(flowLog);
            flowLogger.setLevel(previousLevel);
            flowLog.close();
        }
    }

    public static final class SubscriberSpot implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;

        public SubscriberSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(OrdersHandler.class);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    @ZLinkSpotSubscription(channelName = "events", topic = "orders")
    public static final class OrdersHandler
            implements ZLinkSpotSubscriptionHandler<SubscriberSpot, String> {
        @Override
        public CompletionStage<Void> handle(SubscriberSpot spot, String message) {
            assertEquals("probe", message);
            delivered.countDown();
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PublisherSpot implements ZLinkSpot<ZLinkActor> {
        static final AtomicReference<ZLinkSpotContext> createdContext = new AtomicReference<>();
        private final ZLinkSpotContext context;

        public PublisherSpot(ZLinkSpotContext context) {
            this.context = context;
            createdContext.set(context);
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
