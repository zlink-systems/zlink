package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderAuthorityRepositoryTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.spots.ZLinkSpotRuntimeTesting;
import systems.zlink.framework.spots.*;

import java.util.List;
import java.util.UUID;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

final class SpotPreReadyMulticastTest {
    private static final long WAIT_SECONDS = 3;

    @Test
    void retainsDuringCreateAndRunsOnceAfterReady() throws Exception {
        exercise(false, false);
    }

    @Test
    void retainsDuringInitializeAndRunsOnceAfterReady() throws Exception {
        exercise(true, false);
    }

    @Test
    void rejectedCreateDiscardsMulticast() throws Exception {
        exercise(false, true);
    }

    @Test
    void failedInitializeDiscardsMulticast() throws Exception {
        exercise(true, true);
    }

    @Test
    void initializeAfterPendingCreateResumesOnTheSpotGate() throws Exception {
        exercise(false, false);
        assertTrue(
                HeldSpot.initializeInOwnerTurn,
                "OnInitialize must resume on its Spot execution gate");
    }

    @Test
    void failedConfigureCompletesTheActivationBoundary() throws Exception {
        HeldSpot.reset(false);
        HeldSpot.failConfigure = true;
        try (ZLinkFrameworkRuntime runtime =
                RuntimeTestSupport.startFramework(
                        options(new ZLinkInMemoryLocationStore()),
                        new ZLinkJavaBackendAdapterFactory())) {
            var creating = runtime.spotManager().create("HeldSpot").submit().toCompletableFuture();
            assertThrows(
                    ExecutionException.class, () -> creating.get(WAIT_SECONDS, TimeUnit.SECONDS));
            spotQueue().awaitQuiescence().toCompletableFuture().get(WAIT_SECONDS, TimeUnit.SECONDS);
        }
    }

    private void exercise(boolean initialize, boolean fail) throws Exception {
        HeldSpot.reset(initialize);
        var options = options(new ZLinkInMemoryLocationStore());
        try (ZLinkFrameworkRuntime runtime =
                RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {
            var creating = runtime.spotManager().create("HeldSpot").submit().toCompletableFuture();
            assertTrue(HeldSpot.entered.await(WAIT_SECONDS, TimeUnit.SECONDS));
            List<Message> retained = List.of();
            boolean applicationQueued = false;
            try {
                var observedTopic = new CompletableFuture<ZLinkBackendTopicMessage>();
                try (var observation =
                        ZLinkSpotRuntimeTesting.observeNextTopic(HeldSpot.context, observedTopic)) {
                    HeldSpot.context
                            .outbound()
                            .publish("events", "orders", new Event("held"))
                            .submit()
                            .toCompletableFuture()
                            .get(WAIT_SECONDS, TimeUnit.SECONDS);
                    retained = observedTopic.get(WAIT_SECONDS, TimeUnit.SECONDS).parts();
                }
                applicationQueued =
                        ZLinkSpotRuntimeTesting.hasNoPendingSubscriptions(HeldSpot.context);
                assertEquals(0, HeldSpot.calls.get());
                assertFalse(creating.isDone());
            } finally {
                if (initialize) {
                    if (fail)
                        HeldSpot.initializing.completeExceptionally(
                                new IllegalStateException("initialize failed"));
                    else HeldSpot.initializing.complete(null);
                } else {
                    HeldSpot.creating.complete(
                            fail
                                    ? ZLinkSpotCreateResponse.reject()
                                    : ZLinkSpotCreateResponse.accept());
                }
            }
            if (fail && initialize) {
                assertThrows(
                        ExecutionException.class,
                        () -> creating.get(WAIT_SECONDS, TimeUnit.SECONDS));
            } else {
                var created = creating.get(WAIT_SECONDS, TimeUnit.SECONDS);
                assertEquals(
                        fail ? ZLinkSpotCreateState.REJECTED : ZLinkSpotCreateState.CREATED,
                        created.state());
            }
            if (!fail) {
                HeldSpot.handled.get(WAIT_SECONDS, TimeUnit.SECONDS);
                assertEquals(1, HeldSpot.calls.get());
                // A second publication checks that the Ready path still dispatches normally.
                HeldSpot.context
                        .outbound()
                        .publish("events", "orders", new Event("ready"))
                        .submit()
                        .toCompletableFuture()
                        .get(WAIT_SECONDS, TimeUnit.SECONDS);
                HeldSpot.readyHandled.get(WAIT_SECONDS, TimeUnit.SECONDS);
                assertEquals(2, HeldSpot.calls.get());
            } else {
                assertEquals(0, HeldSpot.calls.get());
            }
            awaitReleased(retained);
            assertTrue(
                    applicationQueued,
                    "pre-Ready payload must be held in the Spot application queue");
        } finally {
            HeldSpot.creating.complete(ZLinkSpotCreateResponse.accept());
            HeldSpot.initializing.complete(null);
        }
        assertEquals(fail ? 0 : 2, HeldSpot.calls.get());
    }

    @Test
    void initializedSpotWaitsForStoreReadyCommit() throws Exception {
        exerciseAtReadyCommit(false);
    }

    @Test
    void lostReservationDiscardsQueuedPayload() throws Exception {
        exerciseAtReadyCommit(true);
    }

    private void exerciseAtReadyCommit(boolean discard) throws Exception {
        HeldSpot.reset(true);
        var store = new HeldReadyStore();
        try (ZLinkFrameworkRuntime runtime =
                RuntimeTestSupport.startFramework(
                        options(store), new ZLinkJavaBackendAdapterFactory())) {
            var creating = runtime.spotManager().create("HeldSpot").submit().toCompletableFuture();
            assertTrue(HeldSpot.entered.await(WAIT_SECONDS, TimeUnit.SECONDS));
            // Use the repository's existing key owner to observe this Spot's Ready write.
            store.key.set(
                    ZLinkProviderAuthorityRepositoryTestAccess.authorityKey(
                            ZLinkAuthorityKeyCodec.spot(HeldSpot.context.spotId())));
            HeldSpot.initializing.complete(null);
            store.entered.get(WAIT_SECONDS, TimeUnit.SECONDS);
            try {
                List<Message> retained;
                var observedTopic = new CompletableFuture<ZLinkBackendTopicMessage>();
                try (var observation =
                        ZLinkSpotRuntimeTesting.observeNextTopic(HeldSpot.context, observedTopic)) {
                    HeldSpot.context
                            .outbound()
                            .publish("events", "orders", new Event("held"))
                            .submit()
                            .toCompletableFuture()
                            .get(WAIT_SECONDS, TimeUnit.SECONDS);
                    retained = observedTopic.get(WAIT_SECONDS, TimeUnit.SECONDS).parts();
                }
                assertEquals(0, HeldSpot.calls.get(), "initialization alone is not Ready");
                assertFalse(creating.isDone());
                store.release.complete(discard);
                if (discard) {
                    assertThrows(
                            ExecutionException.class,
                            () -> creating.get(WAIT_SECONDS, TimeUnit.SECONDS));
                    assertEquals(0, HeldSpot.calls.get());
                } else {
                    assertEquals(
                            ZLinkSpotCreateState.CREATED,
                            creating.get(WAIT_SECONDS, TimeUnit.SECONDS).state());
                    HeldSpot.handled.get(WAIT_SECONDS, TimeUnit.SECONDS);
                    assertEquals(1, HeldSpot.calls.get());
                }
                awaitReleased(retained);
            } finally {
                store.release.complete(false);
            }
        } finally {
            HeldSpot.initializing.complete(null);
            store.release.complete(false);
        }
    }

    private static DefaultZLinkFrameworkOptions options(ZLinkLocationStore store) {
        var options = new DefaultZLinkFrameworkOptions();
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        options.addLocationStore(store);
        var node = options.addRouteMesh("game");
        String suffix = UUID.randomUUID().toString();
        node.listen("inproc://pre-ready-" + suffix).setRoutingId(RoutingId.from(suffix));
        node.channelName("events").server();
        node.objects()
                .server()
                .addSpotFactory("HeldSpot", HeldSpot.class, factory -> factory.disableRelocation());
        return options;
    }

    private static final class HeldReadyStore implements ZLinkLocationStore {
        private final ZLinkLocationStore delegate = new ZLinkInMemoryLocationStore();
        private final AtomicReference<ZLinkStoreKey> key = new AtomicReference<>();
        private final CompletableFuture<Void> entered = new CompletableFuture<>();
        private final CompletableFuture<Boolean> release = new CompletableFuture<>();

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return delegate.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return delegate.scan(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            ZLinkStoreKey target = key.get();
            if (target != null
                    && request.mutations().stream()
                            .anyMatch(
                                    mutation ->
                                            mutation instanceof ZLinkStorePut put
                                                    && put.key().equals(target))
                    && key.compareAndSet(target, null)) {
                entered.complete(null);
                return release.thenCompose(
                        discard -> {
                            if (!discard) return delegate.write(request, cancellation);
                            // Model a revoked reservation through the provider's public atomic
                            // write API.
                            return delegate.write(
                                            new ZLinkStoreWriteRequest(
                                                    List.of(),
                                                    List.of(new ZLinkStoreDelete(target))),
                                            cancellation)
                                    .thenCompose(ignored -> delegate.write(request, cancellation));
                        });
            }
            return delegate.write(request, cancellation);
        }
    }

    private static ZLinkSerialExecutionQueue spotQueue() {
        return ZLinkSpotRuntimeTesting.spotQueue(HeldSpot.context);
    }

    private static void awaitReleased(List<Message> parts) throws Exception {
        assertFalse(parts.isEmpty());
        spotQueue().awaitQuiescence().toCompletableFuture().get(WAIT_SECONDS, TimeUnit.SECONDS);
        for (Message part : parts) {
            assertThrows(
                    IllegalStateException.class,
                    part::refCount,
                    "retained native frame was not released");
        }
    }

    public record Event(String value) {}

    public static final class HeldSpot implements ZLinkSpot<ZLinkActor> {
        static ZLinkSpotContext context;
        static boolean holdInitialize;
        static boolean failConfigure;
        static boolean initializeInOwnerTurn;
        static CountDownLatch entered;
        static CompletableFuture<ZLinkSpotCreateResponse> creating;
        static CompletableFuture<Void> initializing;
        static CompletableFuture<Void> handled;
        static CompletableFuture<Void> readyHandled;
        static final AtomicInteger calls = new AtomicInteger();

        static void reset(boolean initialize) {
            holdInitialize = initialize;
            failConfigure = false;
            initializeInOwnerTurn = false;
            entered = new CountDownLatch(1);
            creating = new CompletableFuture<>();
            initializing = new CompletableFuture<>();
            handled = new CompletableFuture<>();
            readyHandled = new CompletableFuture<>();
            calls.set(0);
        }

        public HeldSpot(ZLinkSpotContext value) {
            context = value;
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

        @Override
        public void configure() {
            context.handlers().addSubscribe(Subscription.class, "events", "orders");
            if (failConfigure) throw new IllegalStateException("configure failed");
        }

        @Override
        public CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request) {
            if (holdInitialize)
                return CompletableFuture.completedFuture(ZLinkSpotCreateResponse.accept());
            entered.countDown();
            return creating;
        }

        @Override
        public CompletionStage<Void> onInitialize() {
            initializeInOwnerTurn = spotQueue().isCurrent();
            if (!holdInitialize) return CompletableFuture.completedFuture(null);
            entered.countDown();
            return initializing;
        }
    }

    public static final class Subscription
            implements ZLinkSpotSubscriptionHandler<HeldSpot, Event> {
        @Override
        public CompletionStage<Void> handle(HeldSpot spot, Event event) {
            HeldSpot.calls.incrementAndGet();
            (event.value().equals("held") ? HeldSpot.handled : HeldSpot.readyHandled)
                    .complete(null);
            return CompletableFuture.completedFuture(null);
        }
    }
}
