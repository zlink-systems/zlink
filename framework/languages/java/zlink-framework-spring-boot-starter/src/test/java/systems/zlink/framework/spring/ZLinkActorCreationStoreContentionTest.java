package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;
import org.springframework.context.annotation.AnnotationConfigApplicationContext;
import org.springframework.context.annotation.Configuration;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;
import systems.zlink.framework.spots.ZLinkActorCreateResponse;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.testsupport.ZLinkHostServing;
import systems.zlink.framework.testsupport.ZLinkLocationContentionFixture;
import systems.zlink.framework.testsupport.ZLinkTestRedis;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkActorCreationStoreContentionTest {
    @ParameterizedTest
    @CsvSource({"in-memory, unlimited", "Redis, at-limit"})
    void distinctActorCreationsUseTwoWritesWithoutConflicts(String provider, String caseId)
            throws Exception {
        var scenario = ZLinkLocationContentionFixture.scenario("distinctActorCreations");
        var expected = scenario.required("expected");
        var settings = scenario.required("cases").required(caseId);
        int actorLimit = settings.required("actorLimit").asInt();
        int creationCount = scenario.required("actorCount").asInt();
        String idPrefix = scenario.required("actorIdPrefix").asText();
        String actorType = scenario.required("actorType").asText();
        String mesh = scenario.required("mesh").asText();
        RoutingId rid = RoutingId.from(mesh);
        String capacity = "zlink:v12:capacity\0" + mesh + "\0" + rid.toHex() + "\0actors\0";
        var active = new ZLinkStoreKey(capacity + "active");
        var pending = new ZLinkStoreKey(capacity + "pending");
        try (var redis =
                provider.equals("Redis")
                        ? new ZLinkRedisLocationStore(
                                new ZLinkRedisLocationOptions()
                                        .setConnectionString(ZLinkTestRedis.endpoint())
                                        .setKeyPrefix("creation-contention:" + UUID.randomUUID()))
                        : null) {
            var store =
                    new RecordingStore(
                            redis == null ? new ZLinkInMemoryProviderLocationStore() : redis,
                            actorLimit,
                            creationCount,
                            "authority\0actor\0" + idPrefix,
                            active,
                            pending);
            var callbacks = new CreationCallbacks();
            try (var context = start(store, mesh, rid, actorType, callbacks)) {
                var runtime = context.getBean(ZLinkFrameworkRuntime.class);
                ZLinkHostServing.await(runtime);
                var callsBefore = store.calls();
                var results =
                        new ArrayList<CompletableFuture<ZLinkActorCreateResult>>(creationCount);
                for (int index = 0; index < creationCount; index++) {
                    results.add(
                            runtime.actorManager()
                                    .create(idPrefix + index, actorType)
                                    .submit()
                                    .toCompletableFuture());
                }
                try {
                    CompletableFuture.allOf(results.toArray(CompletableFuture[]::new))
                            .get(10, TimeUnit.SECONDS);
                } finally {
                    var calls = store.calls().since(callsBefore);
                    System.out.printf(
                            "%s: completed=%d actorIdsWritten=%d reads=%d scans=%d totalWrites=%d"
                                    + " actorWrites=%d conflicts=%d capacityConditions=%d%n",
                            provider,
                            results.stream().filter(CompletableFuture::isDone).count(),
                            store.writesById.size(),
                            calls.reads(),
                            calls.scans(),
                            calls.writes(),
                            store.writesById.values().stream().mapToInt(AtomicInteger::get).sum(),
                            store.conflicts.get(),
                            store.capacityConditions.get());
                    store.releaseReady.complete(null);
                }
                for (var result : results) {
                    assertInstanceOf(ZLinkActorCreateResult.Created.class, result.join());
                }
                assertEquals(
                        expected.required("writeConflicts").asInt(),
                        store.conflicts.get(),
                        "distinct actor writes must not conflict");
                int createdActors = expected.required("createdActors").asInt();
                assertEquals(createdActors, store.writesById.size());
                int writesPerActor = expected.required("authorityWritesPerActor").asInt();
                assertTrue(
                        store.writesById.values().stream()
                                .allMatch(writes -> writes.get() == writesPerActor),
                        "each actor must execute one Reserve write and one Ready write");
                assertEquals(
                        settings.required("expectedReservationConditions").asInt(),
                        store.capacityConditions.get(),
                        "only bounded reservations may carry capacity conditions");
                assertEquals(store.capacityConditions.get(), store.boundedReservations.get());
                assertEquals(createdActors, store.reservationsWithCapacityAdd.get());
                assertEquals(createdActors, store.commitsWithCapacityAdds.get());
                assertEquals(0, store.detachedCapacityWrites.get());
                assertEquals(createdActors, callbacks.factories.get());
                assertEquals(createdActors, callbacks.admissions.get());
                assertEquals(expected.required("activeActorSlots").asLong(), store.counter(active));
                assertEquals(
                        expected.required("pendingActorSlots").asLong(), store.counter(pending));
                long generation = expected.required("objectGeneration").asLong();
                assertTrue(
                        results.stream()
                                .map(CompletableFuture::join)
                                .map(ZLinkActorCreateResult.Created.class::cast)
                                .allMatch(
                                        created ->
                                                created.actor().objectGeneration() == generation),
                        "each new actor ID starts at the fresh Store generation floor");
            }
        }
    }

    private static AnnotationConfigApplicationContext start(
            RecordingStore store,
            String mesh,
            RoutingId rid,
            String actorType,
            CreationCallbacks callbacks) {
        var context = new AnnotationConfigApplicationContext();
        context.registerBean(CreationCallbacks.class, () -> callbacks);
        context.registerBean(
                ZLinkFrameworkConfigurer.class,
                () ->
                        options -> {
                            options.addLocationStore(store);
                            options.configureLocations()
                                    .setOwnerLeaseFencingMargin(Duration.ofNanos(1));
                            var node = options.addRouteMesh(mesh);
                            node.listen("inproc://creation-contention-" + UUID.randomUUID())
                                    .setRoutingId(rid);
                            node.setActivationConcurrency(store.creationCount)
                                    .setActorCapacity(store.limit);
                            var objects = node.objects().server();
                            objects.addEntrySpot(Entry.class);
                            objects.addActorFactory(
                                    actorType,
                                    Player.class,
                                    Factory.class,
                                    factory -> factory.disableRelocation());
                        });
        context.register(Config.class, ZLinkFrameworkAutoConfiguration.class);
        try {
            context.refresh();
            return context;
        } catch (RuntimeException failure) {
            context.close();
            throw failure;
        }
    }

    @Configuration(proxyBeanMethods = false)
    @EnableZLinkFramework
    static class Config {}

    private record StoreCalls(int reads, int scans, int writes) {
        StoreCalls since(StoreCalls previous) {
            return new StoreCalls(
                    reads - previous.reads, scans - previous.scans, writes - previous.writes);
        }
    }

    private static final class RecordingStore implements ZLinkLocationStore {
        private final ZLinkLocationStore delegate;
        private final int limit;
        private final int creationCount;
        private final String authorityPrefix;
        private final ZLinkStoreKey active;
        private final ZLinkStoreKey pending;
        private final AtomicInteger reads = new AtomicInteger();
        private final AtomicInteger scans = new AtomicInteger();
        private final AtomicInteger writes = new AtomicInteger();
        private final ConcurrentHashMap<ZLinkStoreKey, AtomicInteger> writesById =
                new ConcurrentHashMap<>();
        private final AtomicInteger conflicts = new AtomicInteger();
        private final AtomicInteger readyWrites = new AtomicInteger();
        private final AtomicInteger capacityConditions = new AtomicInteger();
        private final AtomicInteger boundedReservations = new AtomicInteger();
        private final AtomicInteger reservationsWithCapacityAdd = new AtomicInteger();
        private final AtomicInteger commitsWithCapacityAdds = new AtomicInteger();
        private final AtomicInteger detachedCapacityWrites = new AtomicInteger();
        private final CompletableFuture<Void> releaseReady = new CompletableFuture<>();

        RecordingStore(
                ZLinkLocationStore delegate,
                int limit,
                int creationCount,
                String authorityPrefix,
                ZLinkStoreKey active,
                ZLinkStoreKey pending) {
            this.delegate = delegate;
            this.limit = limit;
            this.creationCount = creationCount;
            this.authorityPrefix = authorityPrefix;
            this.active = active;
            this.pending = pending;
        }

        long counter(ZLinkStoreKey key) {
            var read = delegate.read(key, () -> false).toCompletableFuture().join();
            return read instanceof ZLinkStoreReadFound found
                    ? Long.parseLong(new String(found.value().bytes(), StandardCharsets.UTF_8))
                    : 0;
        }

        StoreCalls calls() {
            return new StoreCalls(reads.get(), scans.get(), writes.get());
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            reads.incrementAndGet();
            return delegate.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            writes.incrementAndGet();
            var actorKey =
                    request.mutations().stream()
                            .filter(ZLinkStorePut.class::isInstance)
                            .map(ZLinkStorePut.class::cast)
                            .map(ZLinkStorePut::key)
                            .filter(key -> key.value().startsWith(authorityPrefix))
                            .findFirst();
            if (actorKey.isEmpty()) {
                if (request.mutations().stream()
                        .anyMatch(
                                mutation ->
                                        mutation instanceof ZLinkStoreAdd add
                                                && (add.key().equals(active)
                                                        || add.key().equals(pending)))) {
                    detachedCapacityWrites.incrementAndGet();
                }
                return delegate.write(request, cancellation);
            }
            writesById
                    .computeIfAbsent(actorKey.orElseThrow(), key -> new AtomicInteger())
                    .incrementAndGet();
            boolean ready =
                    request.mutations().stream()
                            .anyMatch(
                                    mutation ->
                                            mutation instanceof ZLinkStorePut put
                                                    && put.key()
                                                            .value()
                                                            .startsWith("creation-terminal\0"));
            request.conditions().stream()
                    .filter(ZLinkStoreSumAtMostCondition.class::isInstance)
                    .map(ZLinkStoreSumAtMostCondition.class::cast)
                    .forEach(
                            sum -> {
                                capacityConditions.incrementAndGet();
                                if (!ready
                                        && sum.keys().size() == 2
                                        && sum.keys().containsAll(List.of(active, pending))
                                        && sum.delta() == 1
                                        && sum.limit() == limit) {
                                    boundedReservations.incrementAndGet();
                                }
                            });
            if (ready
                    && request.mutations().contains(new ZLinkStoreAdd(pending, -1))
                    && request.mutations().contains(new ZLinkStoreAdd(active, 1))) {
                commitsWithCapacityAdds.incrementAndGet();
            }
            if (!ready && request.mutations().contains(new ZLinkStoreAdd(pending, 1))) {
                reservationsWithCapacityAdd.incrementAndGet();
            }
            // Build all initial Ready batches before submitting any of them to the provider.
            if (ready && !releaseReady.isDone()) {
                if (readyWrites.incrementAndGet() == creationCount) {
                    releaseReady.complete(null);
                }
                return releaseReady.thenCompose(ignored -> apply(request, cancellation));
            }
            return apply(request, cancellation);
        }

        private CompletionStage<ZLinkStoreWriteResult> apply(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            return delegate.write(request, cancellation)
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
            scans.incrementAndGet();
            return delegate.scan(request, cancellation);
        }
    }

    public record Player(ZLinkActorContext context) implements ZLinkActor {}

    public static final class CreationCallbacks {
        private final AtomicInteger factories = new AtomicInteger();
        private final AtomicInteger admissions = new AtomicInteger();
    }

    public static final class Factory implements ZLinkActorFactory {
        private final CreationCallbacks callbacks;

        public Factory(CreationCallbacks callbacks) {
            this.callbacks = callbacks;
        }

        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            callbacks.factories.incrementAndGet();
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public record Entry(ZLinkEntrySpotContext context, CreationCallbacks callbacks)
            implements ZLinkEntrySpot<Player> {
        @Override
        public CompletionStage<ZLinkActorCreateResponse> onCreateActor(
                Player actor, ZLinkMessage request) {
            callbacks.admissions.incrementAndGet();
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
