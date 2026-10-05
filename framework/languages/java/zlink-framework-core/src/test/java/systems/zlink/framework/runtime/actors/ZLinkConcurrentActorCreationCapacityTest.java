package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkActorCreateResponse;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkConcurrentActorCreationCapacityTest {
    private static final int SESSION_COUNT = 64;
    private static final AtomicInteger FACTORY_CALLS = new AtomicInteger();
    private static final AtomicInteger CALLBACK_CALLS = new AtomicInteger();

    @RepeatedTest(30)
    void concurrentActorsRebuildSharedCapacityWithoutRepeatingFactories() throws Exception {
        FACTORY_CALLS.set(0);
        CALLBACK_CALLS.set(0);
        var store = new SharedCapacityStore();
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        var node = options.addRouteMesh("1304-java");
        node.listen("inproc://1304-java-" + System.nanoTime())
                .setRoutingId(RoutingId.from("1304-java"));
        var objects = node.objects().server();
        objects.addEntrySpot(Entry.class);
        objects.addActorFactory(
                "1304-player", Player.class, Factory.class, factory -> factory.disableRelocation());
        try (ZLinkFrameworkRuntime runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            List<CompletableFuture<ZLinkActorCreateResult>> results = new ArrayList<>();
            for (int session = 0; session < SESSION_COUNT; session++) {
                results.add(
                        runtime.actorManager()
                                .create("1304-player-" + session, "1304-player")
                                .submit()
                                .toCompletableFuture());
            }
            for (var result : results) {
                assertInstanceOf(
                        ZLinkActorCreateResult.Created.class, result.get(10, TimeUnit.SECONDS));
            }
            assertEquals(SESSION_COUNT, FACTORY_CALLS.get());
            assertEquals(SESSION_COUNT, CALLBACK_CALLS.get());
            assertEquals(SESSION_COUNT, store.initialCommits.get());
            assertTrue(
                    store.conflicts.get() >= SESSION_COUNT - 1,
                    "all initial Ready writes must contend for the same capacity version");
        }
    }

    @Test
    void sameLifecycleDescriptorRepublishRebuildsQualifiedConflictWithoutRepeatingFactory()
            throws Exception {
        FACTORY_CALLS.set(0);
        CALLBACK_CALLS.set(0);
        var store = new SharedCapacityStore(true);
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        var node = options.addRouteMesh("1304-java");
        node.listen("inproc://1304-java-" + System.nanoTime())
                .setRoutingId(RoutingId.from("1304-java"));
        var objects = node.objects().server();
        objects.addEntrySpot(Entry.class);
        objects.addActorFactory(
                "1304-player", Player.class, Factory.class, factory -> factory.disableRelocation());
        try (ZLinkFrameworkRuntime runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            assertInstanceOf(
                    ZLinkActorCreateResult.Created.class,
                    runtime.actorManager()
                            .create("1432-player", "1304-player")
                            .submit()
                            .toCompletableFuture()
                            .get(10, TimeUnit.SECONDS));
            assertEquals(1, FACTORY_CALLS.get());
            assertEquals(1, CALLBACK_CALLS.get());
            assertEquals(1, store.conflicts.get());
        }
    }

    private static final class SharedCapacityStore implements ZLinkLocationStore {
        private static final String CREATION_TERMINAL_PREFIX = "creation-terminal\0";
        private final ZLinkLocationStore inner = new ZLinkInMemoryLocationStore();
        private final AtomicInteger initialCommits = new AtomicInteger();
        private final AtomicInteger conflicts = new AtomicInteger();
        private final CompletableFuture<Void> release = new CompletableFuture<>();

        private final boolean republishDescriptor;

        SharedCapacityStore() {
            this(false);
        }

        SharedCapacityStore(boolean republishDescriptor) {
            this.republishDescriptor = republishDescriptor;
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return inner.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            boolean ready =
                    request.mutations().stream()
                                    .anyMatch(
                                            mutation ->
                                                    mutation instanceof ZLinkStorePut put
                                                            && put.key()
                                                                    .value()
                                                                    .startsWith(
                                                                            CREATION_TERMINAL_PREFIX))
                            && request.mutations().stream()
                                    .noneMatch(ZLinkStoreDelete.class::isInstance);
            if (ready && republishDescriptor && initialCommits.getAndIncrement() == 0) {
                var descriptorCondition =
                        request.conditions().stream()
                                .filter(ZLinkStoreVersionCondition.class::isInstance)
                                .map(ZLinkStoreVersionCondition.class::cast)
                                .filter(
                                        condition ->
                                                condition.key().value().startsWith("mesh-node\0"))
                                .findFirst()
                                .orElseThrow();
                return inner.read(descriptorCondition.key(), cancellation)
                        .thenCompose(
                                read -> {
                                    var found = (ZLinkStoreReadFound) read;
                                    return inner.write(
                                                    new ZLinkStoreWriteRequest(
                                                            List.of(),
                                                            List.of(
                                                                    new ZLinkStorePut(
                                                                            descriptorCondition
                                                                                    .key(),
                                                                            found.value().bytes(),
                                                                            null))),
                                                    cancellation)
                                            .thenCompose(ignored -> apply(request, cancellation));
                                });
            }
            if (ready && !republishDescriptor && !release.isDone()) {
                if (initialCommits.incrementAndGet() == SESSION_COUNT) {
                    release.complete(null);
                }
                return release.thenCompose(ignored -> apply(request, cancellation));
            }
            return apply(request, cancellation);
        }

        private CompletionStage<ZLinkStoreWriteResult> apply(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            return inner.write(request, cancellation)
                    .thenApply(
                            result -> {
                                if (result instanceof ZLinkStoreWriteConflict) {
                                    conflicts.incrementAndGet();
                                }
                                return result;
                            });
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }
    }

    public static final class Player implements ZLinkActor {
        private final ZLinkActorContext context;

        public Player(ZLinkActorContext context) {
            this.context = context;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }
    }

    public static final class Factory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            FACTORY_CALLS.incrementAndGet();
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class Entry implements ZLinkEntrySpot<Player> {
        private final ZLinkEntrySpotContext context;

        public Entry(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<ZLinkActorCreateResponse> onCreateActor(
                Player actor, ZLinkMessage request) {
            CALLBACK_CALLS.incrementAndGet();
            return CompletableFuture.completedFuture(ZLinkActorCreateResponse.accept());
        }

        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
