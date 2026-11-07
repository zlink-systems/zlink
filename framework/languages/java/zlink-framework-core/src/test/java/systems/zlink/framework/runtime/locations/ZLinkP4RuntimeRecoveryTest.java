package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.RepeatedTest;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;

import java.time.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicLong;

final class ZLinkP4RuntimeRecoveryTest {
    private static final Duration MARGIN = Duration.ofNanos(10);

    private static final class MutableClock extends Clock {
        Instant now = Instant.parse("2026-01-01T00:00:00Z");
        volatile Runnable beforeRead;

        public ZoneId getZone() {
            return ZoneOffset.UTC;
        }

        public Clock withZone(ZoneId zone) {
            return this;
        }

        public Instant instant() {
            Runnable action = beforeRead;
            if (action != null) action.run();
            return now;
        }
    }

    private static <T> T await(CompletionStage<T> stage) {
        return stage.toCompletableFuture().join();
    }

    private static final class ProbeStore implements ZLinkLocationStore {
        final ZLinkLocationStore inner;
        final List<ZLinkStoreWriteRequest> writes = new ArrayList<>();
        boolean failScan;
        boolean expireContinuation;
        Runnable onExpire;
        Runnable beforeWrite;

        ProbeStore(ZLinkLocationStore inner) {
            this.inner = inner;
        }

        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            return inner.read(key, cancellation);
        }

        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            writes.add(request);
            var before = beforeWrite;
            beforeWrite = null;
            if (before != null) before.run();
            return inner.write(request, cancellation);
        }

        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request,
                systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
            if (failScan)
                return CompletableFuture.failedFuture(
                        new IllegalStateException("injected scan failure"));
            if (expireContinuation && request.cursor() != null) {
                expireContinuation = false;
                if (onExpire != null) onExpire.run();
                return CompletableFuture.completedFuture(new ZLinkStoreScanExpired());
            }
            return inner.scan(
                    new ZLinkStoreScanRequest(request.prefix(), request.cursor(), 1), cancellation);
        }
    }

    private static final class Fixture {
        final MutableClock clock = new MutableClock();
        final AtomicLong nanos = new AtomicLong();
        final ZLinkInMemoryProviderLocationStore raw =
                new ZLinkInMemoryProviderLocationStore(clock);
        final ProbeStore probe = new ProbeStore(raw);
        final ZLinkLocationRepository repository;
        ZLinkLocationOwnerToken owner;
        ZLinkMeshNodeDescriptorKey node =
                new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node"));

        Fixture(boolean provider) {
            repository =
                    provider
                            ? new ZLinkProviderLocationRepository(probe, nanos::get, MARGIN)
                            : new ZLinkInMemoryLocationStore(clock, nanos::get, MARGIN);
            owner =
                    assertInstanceOf(
                                    ZLinkOwnerLeaseClaimed.class,
                                    await(
                                            repository.claimOwnerLease(
                                                    "owner", Duration.ofSeconds(30))))
                            .token();
            publish(1);
        }

        long publishedLifecycle;
        long descriptorRevision;

        void publish(long lifecycle) {
            boolean renew = lifecycle == publishedLifecycle;
            var d =
                    new ZLinkMeshNodeDescriptor(
                            node.meshName(),
                            node.rid(),
                            lifecycle,
                            ++descriptorRevision,
                            "tcp://127.0.0.1:7100",
                            Map.of(),
                            1,
                            List.of(
                                    new ZLinkObjectCapability(
                                            ZLinkPlacementObjectKind.USER_SPOT,
                                            "room",
                                            ZLinkObjectMaintenancePolicyKind.DISABLED,
                                            false,
                                            0),
                                    new ZLinkObjectCapability(
                                            ZLinkPlacementObjectKind.ACTOR,
                                            "player",
                                            ZLinkObjectMaintenancePolicyKind.DISABLED,
                                            false,
                                            0)),
                            ZLinkMeshNodeObjectRole.SERVER,
                            Optional.of("entry-" + node.meshName()),
                            100,
                            new ZLinkPlacementCapacity(
                                    new ZLinkCapacityUsage(0, 0, 10000),
                                    new ZLinkCapacityUsage(0, 0, 10000),
                                    List.of()),
                            new ZLinkActivationConcurrency(0, 10000),
                            Optional.empty(),
                            ZLinkFrameworkRuntimeState.SERVING,
                            "security",
                            owner.ownerId(),
                            owner.leaseGeneration(),
                            clock.instant());
            assertEquals(
                    ZLinkLocationWriteStatus.STORED,
                    await(
                                    repository.updateMeshNode(
                                            d,
                                            renew
                                                    ? ZLinkLocationWriteIntent.RENEW
                                                    : ZLinkLocationWriteIntent.NEW_CLAIM))
                            .status());
            publishedLifecycle = lifecycle;
        }

        void expireAndClaim() {
            clock.now = clock.now.plus(Duration.ofHours(2));
            owner =
                    assertInstanceOf(
                                    ZLinkOwnerLeaseClaimed.class,
                                    await(repository.claimOwnerLease("owner", Duration.ofHours(1))))
                            .token();
        }

        ZLinkAuthoritySnapshot read(String key) {
            return assertInstanceOf(
                    ZLinkAuthoritySnapshot.class, await(repository.read(key, () -> false)));
        }

        ZLinkObjectReservation reserve(String key, boolean actor, byte[] payload) {
            return assertInstanceOf(
                            ZLinkObjectReserved.class,
                            await(
                                    repository.reserve(
                                            new ZLinkObjectReservationRequest(
                                                    actor
                                                            ? ZLinkPlacementObjectKind.ACTOR
                                                            : ZLinkPlacementObjectKind.USER_SPOT,
                                                    key,
                                                    actor ? "player" : "room",
                                                    "intent-" + key,
                                                    new byte[32],
                                                    0,
                                                    node,
                                                    1,
                                                    owner,
                                                    payload,
                                                    actor
                                                            ? ZLinkPlacementCapacityBundle.actor(1)
                                                            : ZLinkPlacementCapacityBundle.spot(
                                                                    ZLinkPlacementObjectKind
                                                                            .USER_SPOT,
                                                                    "room",
                                                                    1)),
                                            () -> false)))
                    .reservation();
        }

        ZLinkAuthoritySnapshot spot(String id) {
            String key = ZLinkAuthorityKeyCodec.spot(id);
            byte[] payload =
                    new ZLinkServiceAuthorityPayloadCodec()
                            .encodeUser(
                                    ZLinkServiceAuthorityPayloadCodec.State.READY,
                                    "room",
                                    id,
                                    owner.ownerId(),
                                    owner.leaseGeneration(),
                                    node.meshName(),
                                    node.rid(),
                                    1);
            var reservation = reserve(key, false, payload);
            assertEquals(
                    ZLinkObjectCommitResult.COMMITTED,
                    await(repository.commit(reservation, payload, () -> false)));
            return read(key);
        }

        ZLinkAuthoritySnapshot actor(String id, String spot, long generation) {
            String key = ZLinkAuthorityKeyCodec.actor(id);
            byte[] payload =
                    new ZLinkActorAuthorityPayloadCodec()
                            .encode(
                                    ZLinkActorAuthorityPayloadCodec.State.READY,
                                    "player",
                                    id,
                                    spot,
                                    generation,
                                    2,
                                    owner.ownerId(),
                                    owner.leaseGeneration(),
                                    node.meshName(),
                                    node.rid(),
                                    1);
            var reservation = reserve(key, true, payload);
            assertEquals(
                    ZLinkObjectCommitResult.COMMITTED,
                    await(repository.commit(reservation, payload, () -> false)));
            return read(key);
        }

        ZLinkAuthorityWriteResult preserve(String key, ZLinkAuthoritySnapshot before) {
            return await(
                    repository.compareExchange(
                            key,
                            new ZLinkAuthorityExpectFound(before.storeVersion()),
                            ZLinkAuthorityPut.preserve(before, owner),
                            () -> false));
        }

        boolean cleanup(String key, ZLinkAuthoritySnapshot before) {
            return await(repository.cleanupDiscarded(key, before, owner, () -> false));
        }

        ZLinkPlacementCapacity capacity() {
            return await(repository.listMeshNodes("game", ZLinkPageRequest.firstPage()))
                    .items()
                    .getFirst()
                    .capacity();
        }
    }

    private static final class RuntimeFixture implements AutoCloseable {
        final Fixture f;
        final ZLinkLocationFirstHeartbeatTest.RecordingScheduler scheduler =
                new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
        final ZLinkLocationRuntime runtime;
        boolean unavailable;
        CompletableFuture<Void> publication = CompletableFuture.completedFuture(null);
        final Map<String, CompletableFuture<ZLinkAuthorityWriteResult>> heldWrites =
                new HashMap<>();
        final Map<String, ZLinkAuthorityWriteResult> submitted = new ConcurrentHashMap<>();
        final Map<String, CompletableFuture<Void>> beforeWrites = new HashMap<>();
        // Written by whichever thread runs the store call and read by the test thread, so they are
        // volatile and only incremented under the fixture monitor, which also wakes awaitWrites.
        volatile int publications;
        volatile int reads;
        volatile int writes;
        volatile int cleanups;
        volatile int descriptorReads;
        int clockReads;
        int collected;
        final Set<String> loseWrites = new HashSet<>();
        final Set<String> failWrites = new HashSet<>();
        final Map<String, CompletableFuture<ZLinkAuthorityReadResult>> heldReads = new HashMap<>();
        boolean failCleanup;
        boolean cleanupError;
        boolean synchronousReadFailure;
        Runnable afterWrite;
        ZLinkLocationWriteStatus publicationStatus = ZLinkLocationWriteStatus.STORED;

        RuntimeFixture(boolean provider) {
            f = new Fixture(provider);
            var adapter =
                    new systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter() {
                        public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                                String owner, Duration ttl) {
                            return f.repository.claimOwnerLease(owner, ttl);
                        }

                        public CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(
                                String owner) {
                            return f.repository.readOwnerLease(owner);
                        }

                        public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
                                ZLinkLocationOwnerToken owner, Duration ttl) {
                            return unavailable
                                    ? CompletableFuture.failedFuture(
                                            new IllegalStateException("Store unavailable"))
                                    : f.repository.renewOwnerLease(owner, ttl);
                        }

                        public CompletionStage<ZLinkAuthorityReadResult> read(
                                String key, ZLinkStoreCancellation cancellation) {
                            countRead();
                            if (synchronousReadFailure)
                                throw new IllegalStateException(
                                        "synchronous provider read failure");
                            var held = heldReads.get(key);
                            return held == null ? f.repository.read(key, cancellation) : held;
                        }

                        public CompletionStage<ZLinkAuthorityWriteResult> compareExchange(
                                String key,
                                ZLinkAuthorityExpectation expected,
                                ZLinkAuthorityMutation mutation,
                                ZLinkStoreCancellation cancellation) {
                            assertTrue(publications > 0, "descriptor submission precedes Preserve");
                            countWrite();
                            if (failWrites.remove(key))
                                return CompletableFuture.failedFuture(
                                        new TimeoutException("write not applied"));
                            return beforeWrites
                                    .getOrDefault(key, CompletableFuture.completedFuture(null))
                                    .thenCompose(
                                            ignored ->
                                                    f.repository.compareExchange(
                                                            key, expected, mutation, cancellation))
                                    .thenCompose(
                                            result -> {
                                                submitted.put(key, result);
                                signal();
                                                if (afterWrite != null) afterWrite.run();
                                                if (loseWrites.remove(key))
                                                    return CompletableFuture.failedFuture(
                                                            new TimeoutException(
                                                                    "applied response lost"));
                                                return heldWrites.containsKey(key)
                                                        ? heldWrites.get(key)
                                                        : CompletableFuture.completedFuture(result);
                                            });
                        }

                        public CompletionStage<Boolean> cleanupDiscarded(
                                String key,
                                ZLinkAuthoritySnapshot snapshot,
                                ZLinkLocationOwnerToken owner,
                                ZLinkStoreCancellation cancellation) {
                            countCleanup();
                            if (cleanupError)
                                return CompletableFuture.failedFuture(
                                        new TimeoutException("cleanup provider unavailable"));
                            return failCleanup
                                    ? CompletableFuture.completedFuture(false)
                                    : f.repository.cleanupDiscarded(
                                            key, snapshot, owner, cancellation);
                        }

                        public CompletionStage<ZLinkLocationPage<ZLinkMeshNodeDescriptor>>
                                listMeshNodes(String mesh, ZLinkPageRequest page) {
                            countDescriptorRead();
                            return f.repository.listMeshNodes(mesh, page);
                        }

                        public CompletionStage<Long> removeAllByOwner(
                                ZLinkLocationOwnerToken owner) {
                            return f.repository.removeAllByOwner(owner);
                        }

                        public CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(
                                ZLinkLocationOwnerToken owner) {
                            return f.repository.releaseOwnerLease(owner);
                        }
                    };
            runtime =
                    new ZLinkLocationRuntime(
                            ZLinkRegisteredLocationStores.fromUnified(adapter),
                            "owner",
                            Duration.ofSeconds(30),
                            Duration.ofSeconds(5),
                            Duration.ofSeconds(3),
                            Duration.ofSeconds(5),
                            () -> {
                                clockReads++;
                                return f.nanos.get();
                            },
                            scheduler,
                            new systems.zlink.framework.runtime.internal.execution.ZLinkStateLane(
                                    Runnable::run));
            runtime.setStoreOutageRetention(true);
            await(runtime.start(f.node.rid()));
            runtime.setOwnerLeaseRecoveryListener(
                    open -> {
                        countPublication();
                        f.owner = runtime.currentOwnerToken();
                        return runtime.writeDescriptor(
                                        f.node,
                                        () -> {
                                            f.publish(1);
                                            return publication
                                                    .thenApply(
                                                            ignored ->
                                                                    new ZLinkLocationWriteResult(
                                                                            publicationStatus,
                                                                            1,
                                                                            f.clock.instant()))
                                                    .thenApply(RuntimeFixture::confirmPublication);
                                        })
                                .thenApply(ignored -> null);
                    });
        }

        private synchronized void signal() {
            notifyAll();
        }

        synchronized void countRead() {
            reads++;
            notifyAll();
        }

        synchronized void countWrite() {
            writes++;
            notifyAll();
        }

        synchronized void countCleanup() {
            cleanups++;
            notifyAll();
        }

        synchronized void countDescriptorRead() {
            descriptorReads++;
            notifyAll();
        }

        synchronized void countPublication() {
            publications++;
            notifyAll();
        }

        /** Waits until the Preserve CAS count reaches {@code expected}; fails after the deadline. */
        synchronized void awaitWrites(int expected) {
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30);
            while (writes < expected) {
                long left = deadline - System.nanoTime();
                if (left <= 0)
                    fail("timed out waiting for " + expected + " CAS submissions, saw " + writes);
                try {
                    TimeUnit.NANOSECONDS.timedWait(this, left);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    fail("interrupted waiting for " + expected + " CAS submissions");
                }
            }
        }

        /** Waits until the CAS for {@code key} has been applied by the store and returns it. */
        synchronized ZLinkAuthorityWriteResult awaitSubmitted(String key) {
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30);
            while (!submitted.containsKey(key)) {
                long left = deadline - System.nanoTime();
                if (left <= 0) fail("timed out waiting for the CAS of " + key);
                try {
                    TimeUnit.NANOSECONDS.timedWait(this, left);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    fail("interrupted waiting for the CAS of " + key);
                }
            }
            return submitted.get(key);
        }

        static ZLinkLocationWriteResult confirmPublication(ZLinkLocationWriteResult result) {
            if (result.status() != ZLinkLocationWriteStatus.STORED)
                throw new systems.zlink.framework.errors.ZLinkFrameworkException(
                        systems.zlink.framework.errors.ZLinkFrameworkErrorKind.NOT_CONFIGURED,
                        "descriptor publication rejected");
            return result;
        }

        ZLinkLocationRuntime.Execution bind(
                String id,
                java.util.concurrent.atomic.AtomicInteger disposed,
                java.util.concurrent.atomic.AtomicInteger applied) {
            f.spot(id);
            var execution =
                    runtime.execution(
                            ZLinkAuthorityKeyCodec.spot(id),
                            () -> {
                                collected++;
                                return true;
                            },
                            snapshot -> {
                                applied.incrementAndGet();
                                return CompletableFuture.completedFuture(null);
                            },
                            () -> null,
                            disposed::incrementAndGet);
            await(execution.ready());
            return execution;
        }

        void outage() {
            f.nanos.incrementAndGet();
            unavailable = true;
            assertFalse(await(runtime.renewOwnerLeaseOnce()));
            unavailable = false;
        }

        void at(long seconds, long offset) {
            f.nanos.set(Duration.ofSeconds(seconds).toNanos() + offset);
            f.clock.now =
                    Instant.parse("2026-01-01T00:00:00Z").plusSeconds(seconds).plusNanos(offset);
        }

        public void close() {
            runtime.close();
        }
    }

    @RepeatedTest(100)
    void sameToken() {
        for (boolean p : new boolean[] {false, true}) sameToken(p);
    }

    @RepeatedTest(100)
    void independentPublicationAndAuthority() {
        for (boolean p : new boolean[] {false, true}) independentPublicationAndAuthority(p);
    }

    @RepeatedTest(100)
    void beforeCutoff() {
        for (boolean p : new boolean[] {false, true}) cutoff(p, -1);
    }

    @RepeatedTest(100)
    void atCutoff() {
        for (boolean p : new boolean[] {false, true}) cutoff(p, 0);
    }

    @RepeatedTest(100)
    void afterCutoff() {
        for (boolean p : new boolean[] {false, true}) cutoff(p, 1);
    }

    @RepeatedTest(100)
    void inclusiveSubmissionBudget() {
        for (boolean p : new boolean[] {false, true}) submissionBudget(p, 0);
    }

    @RepeatedTest(100)
    void exceededSubmissionBudget() {
        for (boolean p : new boolean[] {false, true}) submissionBudget(p, 1);
    }

    void sameToken(boolean provider) {
        try (var f = new RuntimeFixture(provider)) {
            var disposed = new java.util.concurrent.atomic.AtomicInteger();
            var applied = new java.util.concurrent.atomic.AtomicInteger();
            var execution = f.bind("a", disposed, applied);
            f.outage();
            assertTrue(await(f.runtime.renewOwnerLeaseOnce()));
            assertEquals(
                    2,
                    f.reads,
                    "Ready capture and same-token recovery validate the preserved authority");
            assertEquals(0, f.writes);
            assertEquals(0, disposed.get());
            execution.check(true);
            assertNull(f.runtime.ownerLeaseSnapshot().outageObservedAtNanos());
        }
    }

    void independentPublicationAndAuthority(boolean provider) {
        try (var f = new RuntimeFixture(provider)) {
            var disposed = new java.util.concurrent.atomic.AtomicInteger();
            var appliedA = new java.util.concurrent.atomic.AtomicInteger();
            var appliedB = new java.util.concurrent.atomic.AtomicInteger();
            var a = f.bind("a", disposed, appliedA);
            var b = f.bind("b", disposed, appliedB);
            String key = ZLinkAuthorityKeyCodec.spot("a");
            f.heldWrites.put(key, new CompletableFuture<>());
            f.outage();
            f.at(35, 0);
            f.publication = new CompletableFuture<>();
            var renewal = f.runtime.renewOwnerLeaseOnce();
            f.awaitWrites(2);
            assertEquals(2, f.writes);
            assertThrows(IllegalStateException.class, () -> a.check(true));
            assertThrows(IllegalStateException.class, () -> b.check(true));
            assertEquals(0, appliedB.get(), "CAS without descriptor confirmation cannot reopen");
            f.publication.complete(null);
            b.check(true);
            assertThrows(IllegalStateException.class, () -> a.check(true));
            f.heldWrites.get(key).complete(f.awaitSubmitted(key));
            assertTrue(await(renewal));
            a.check(true);
            assertEquals(1, appliedA.get());
            assertEquals(1, appliedB.get());
            assertEquals(0, disposed.get());
        }
    }

    void cutoff(boolean provider, long offset) {
        try (var f = new RuntimeFixture(provider)) {
            var disposed = new java.util.concurrent.atomic.AtomicInteger();
            var applied = new java.util.concurrent.atomic.AtomicInteger();
            var a = f.bind("a", disposed, applied);
            f.outage();
            f.at(35, 0);
            f.publication = new CompletableFuture<>();
            var renewal = f.runtime.renewOwnerLeaseOnce();
            f.awaitWrites(1);
            assertEquals(1, f.writes);
            f.at(40, offset);
            f.publication.complete(null);
            await(renewal);
            assertEquals(offset < 0 ? 1 : 0, applied.get());
            assertEquals(offset < 0 ? 0 : 1, disposed.get());
            if (offset >= 0) assertTrue(f.cleanups > 0);
        }
    }

    void submissionBudget(boolean provider, long offset) {
        try (var f = new RuntimeFixture(provider)) {
            var disposed = new java.util.concurrent.atomic.AtomicInteger();
            var applied = new java.util.concurrent.atomic.AtomicInteger();
            f.bind("a", disposed, applied);
            f.outage();
            f.at(37, offset);
            await(f.runtime.renewOwnerLeaseOnce());
            assertEquals(offset == 0 ? 1 : 0, f.writes, "start + timeout = T is inclusive");
            assertEquals(offset == 0 ? 1 : 0, applied.get());
        }
    }

    @RepeatedTest(100)
    void lostCasResponseUsesExactReadAndNeverResubmits() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new RuntimeFixture(provider)) {
                var disposed = new java.util.concurrent.atomic.AtomicInteger();
                var applied = new java.util.concurrent.atomic.AtomicInteger();
                var a = f.bind("a", disposed, applied);
                var key = ZLinkAuthorityKeyCodec.spot("a");
                f.loseWrites.add(key);
                f.outage();
                f.at(35, 0);
                assertTrue(await(f.runtime.renewOwnerLeaseOnce()));
                assertEquals(1, f.writes);
                assertEquals(
                        3, f.reads, "capture, initial exact read and lost-response confirmation");
                assertEquals(1, applied.get());
                a.check(true);
                assertEquals(0, disposed.get());
            }
        }
    }

    @RepeatedTest(100)
    void unchangedIndeterminateCasStaysPausedUntilTheExistingHeartbeatRetries() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new RuntimeFixture(provider)) {
                var disposed = new java.util.concurrent.atomic.AtomicInteger();
                var applied = new java.util.concurrent.atomic.AtomicInteger();
                var a = f.bind("a", disposed, applied);
                f.failWrites.add(ZLinkAuthorityKeyCodec.spot("a"));
                f.outage();
                f.at(35, 0);
                await(f.runtime.renewOwnerLeaseOnce());
                assertTrue(a.paused());
                assertEquals(1, f.writes);
                assertEquals(0, applied.get());
                assertEquals(0, disposed.get());
                await(f.runtime.renewOwnerLeaseOnce());
                assertFalse(a.paused());
                assertEquals(2, f.writes);
                assertEquals(1, applied.get());
            }
        }
    }

    @RepeatedTest(100)
    void confirmedConflictDiscardsOnlyThatObject() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new RuntimeFixture(provider)) {
                var discardedA = new java.util.concurrent.atomic.AtomicInteger();
                var discardedB = new java.util.concurrent.atomic.AtomicInteger();
                var appliedA = new java.util.concurrent.atomic.AtomicInteger();
                var appliedB = new java.util.concurrent.atomic.AtomicInteger();
                var a = f.bind("a", discardedA, appliedA);
                var b = f.bind("b", discardedB, appliedB);
                var key = ZLinkAuthorityKeyCodec.spot("a");
                var original = f.f.read(key);
                await(
                        f.f.repository.compareExchange(
                                key,
                                new ZLinkAuthorityExpectFound(original.storeVersion()),
                                new ZLinkAuthorityPut(original.payload()),
                                () -> false));
                f.outage();
                f.at(35, 0);
                await(f.runtime.renewOwnerLeaseOnce());
                assertEquals(1, discardedA.get());
                assertEquals(0, appliedA.get());
                assertEquals(0, discardedB.get());
                assertEquals(1, appliedB.get());
                b.check(true);
                assertThrows(
                        systems.zlink.framework.errors.ZLinkFrameworkException.class,
                        () -> a.check(true));
            }
        }
    }

    @RepeatedTest(100)
    void pausedQueueKeepsAcceptedMessagesAndContinuations() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new RuntimeFixture(provider)) {
                var applied = new java.util.concurrent.atomic.AtomicInteger();
                var a = f.bind("a", new java.util.concurrent.atomic.AtomicInteger(), applied);
                var queue =
                        new systems.zlink.framework.execution.ZLinkSerialExecutionQueue(
                                Runnable::run,
                                systems.zlink.framework.execution.ZLinkExecutionLanePolicy.spot());
                queue.executionPolicy(a);
                var calls = new java.util.concurrent.atomic.AtomicInteger();
                f.outage();
                f.at(35, 0);
                f.publication = new CompletableFuture<>();
                var recovery = f.runtime.renewOwnerLeaseOnce();
                var pending =
                        queue.enqueue(
                                () -> {
                                    calls.incrementAndGet();
                                    return CompletableFuture.completedFuture(null);
                                },
                                null);
                assertFalse(pending.toCompletableFuture().isDone());
                assertEquals(0, calls.get());
                // Infrastructure still progresses through the same gate while applications wait.
                await(queue.enqueueLifecycleBarrier(() -> CompletableFuture.completedFuture(null)));
                f.publication.complete(null);
                await(recovery);
                await(pending);
                assertEquals(1, calls.get());
                assertEquals(1, applied.get());
                queue.close();
            }
        }
    }

    @RepeatedTest(100)
    void sameTokenChecksDescriptorAndSnapshotBeforeReopening() {
        for (boolean provider : new boolean[] {false, true}) {
            for (boolean descriptorChanged : new boolean[] {false, true}) {
                try (var f = new RuntimeFixture(provider)) {
                    var disposed = new java.util.concurrent.atomic.AtomicInteger();
                    var applied = new java.util.concurrent.atomic.AtomicInteger();
                    f.bind("a", disposed, applied);
                    if (descriptorChanged) {
                        f.runtime.setOwnerLeaseRecoveryListener(
                                ignored -> CompletableFuture.completedFuture(null));
                        await(f.f.repository.removeMeshNode(f.f.node, f.f.owner));
                        f.f.publish(2);
                    } else {
                        var key = ZLinkAuthorityKeyCodec.spot("a");
                        var original = f.f.read(key);
                        await(
                                f.f.repository.compareExchange(
                                        key,
                                        new ZLinkAuthorityExpectFound(original.storeVersion()),
                                        new ZLinkAuthorityPut(original.payload()),
                                        () -> false));
                    }
                    f.outage();
                    await(f.runtime.renewOwnerLeaseOnce());
                    assertEquals(1, f.descriptorReads);
                    assertEquals(0, f.writes);
                    assertEquals(1, disposed.get());
                    assertEquals(0, applied.get());
                }
            }
        }
    }

    @RepeatedTest(100)
    void rejectedDescriptorAndLateConfirmationNeverReopen() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new RuntimeFixture(provider)) {
                var disposed = new java.util.concurrent.atomic.AtomicInteger();
                var applied = new java.util.concurrent.atomic.AtomicInteger();
                var a = f.bind("a", disposed, applied);
                f.publicationStatus = ZLinkLocationWriteStatus.REJECTED_CONFLICT;
                f.outage();
                f.at(35, 0);
                await(f.runtime.renewOwnerLeaseOnce());
                assertEquals(1, disposed.get());
                assertEquals(0, applied.get());
                assertThrows(
                        systems.zlink.framework.errors.ZLinkFrameworkException.class,
                        () -> a.check(true));
                assertTrue(f.cleanups > 0);
            }
        }
    }

    @RepeatedTest(100)
    void delayedAuthorityReadCannotUseAnEarlierSubmissionBudget() {
        for (boolean provider : new boolean[] {false, true}) {
            for (long offset : new long[] {0, 1}) {
                try (var f = new RuntimeFixture(provider)) {
                    var applied = new java.util.concurrent.atomic.AtomicInteger();
                    f.bind("a", new java.util.concurrent.atomic.AtomicInteger(), applied);
                    String key = ZLinkAuthorityKeyCodec.spot("a");
                    var exact = f.f.read(key);
                    var held = new CompletableFuture<ZLinkAuthorityReadResult>();
                    f.heldReads.put(key, held);
                    f.outage();
                    f.at(35, 0);
                    var renewal = f.runtime.renewOwnerLeaseOnce();
                    f.at(37, offset);
                    held.complete(exact);
                    await(renewal);
                    assertEquals(offset == 0 ? 1 : 0, f.writes);
                    assertEquals(offset == 0 ? 1 : 0, applied.get());
                }
            }
        }
    }

    @RepeatedTest(100)
    void fiveThousandOwnersUseOneDescriptorScanAndConstantWorkPerObject() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new RuntimeFixture(provider)) {
                var disposed = new java.util.concurrent.atomic.AtomicInteger();
                var applied = new java.util.concurrent.atomic.AtomicInteger();
                var owners = new ArrayList<ZLinkLocationRuntime.Execution>();
                for (int i = 0; i < 5000; i++) owners.add(f.bind("a" + i, disposed, applied));
                int normalClockReads = f.clockReads;
                for (var owner : owners) owner.check(true);
                assertEquals(
                        normalClockReads, f.clockReads, "ordinary turns do not consult the clock");
                f.outage();
                assertEquals(5000, f.collected, "one capture per live owner");
                int collection = f.collected;
                for (var owner : owners) owner.check(true);
                assertEquals(
                        collection, f.collected, "outage checks look up only their owner record");
                await(f.runtime.renewOwnerLeaseOnce());
                assertEquals(10000, f.reads, "one capture and one recovery read per object");
                assertEquals(5000, applied.get());
                assertEquals(0, f.writes);
                assertEquals(
                        1, f.descriptorReads, "one scan per mesh, shared by all owner records");
                assertEquals(
                        collection, f.collected, "completion never recollects the live owner set");
                normalClockReads = f.clockReads;
                for (var owner : owners) owner.check(true);
                assertEquals(normalClockReads, f.clockReads);
                assertEquals(0, disposed.get());
            }
        }
    }

    @RepeatedTest(100)
    void lateCasAndLateExactConfirmationCleanUpInsteadOfReopening() {
        for (boolean provider : new boolean[] {false, true}) {
            for (boolean lost : new boolean[] {false, true}) {
                for (long offset : new long[] {-1, 0, 1}) {
                    try (var f = new RuntimeFixture(provider)) {
                        var disposed = new java.util.concurrent.atomic.AtomicInteger();
                        var applied = new java.util.concurrent.atomic.AtomicInteger();
                        f.bind("a", disposed, applied);
                        String key = ZLinkAuthorityKeyCodec.spot("a");
                        f.outage();
                        f.at(35, 0);
                        if (lost) {
                            f.loseWrites.add(key);
                            f.afterWrite = () -> f.at(40, offset);
                            await(f.runtime.renewOwnerLeaseOnce());
                        } else {
                            var held = new CompletableFuture<ZLinkAuthorityWriteResult>();
                            f.heldWrites.put(key, held);
                            var renewal = f.runtime.renewOwnerLeaseOnce();
                            f.at(40, offset);
                            held.complete(f.awaitSubmitted(key));
                            await(renewal);
                        }
                        assertEquals(1, f.writes);
                        assertEquals(offset < 0 ? 1 : 0, applied.get());
                        assertEquals(offset < 0 ? 0 : 1, disposed.get());
                        assertEquals(offset < 0 ? 0 : 1, f.cleanups);
                        if (offset >= 0)
                            assertInstanceOf(
                                    ZLinkAuthorityMissing.class,
                                    await(f.f.repository.read(key, () -> false)));
                    }
                }
            }
        }
    }

    @RepeatedTest(100)
    void localMemberWaitsForItsSpotWithoutHoldingTheHeartbeat() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new RuntimeFixture(provider)) {
                var disposed = new java.util.concurrent.atomic.AtomicInteger();
                var spotApplied = new java.util.concurrent.atomic.AtomicInteger();
                var actorApplied = new java.util.concurrent.atomic.AtomicInteger();
                var parent = f.bind("parent", disposed, spotApplied);
                String parentKey = ZLinkAuthorityKeyCodec.spot("parent");
                var parentAuthority = f.f.read(parentKey);
                f.f.actor("member", "parent", parentAuthority.objectGeneration());
                var queue =
                        new systems.zlink.framework.execution.ZLinkSerialExecutionQueue(
                                Runnable::run,
                                systems.zlink.framework.execution.ZLinkExecutionLanePolicy.spot());
                var member =
                        f.runtime.execution(
                                ZLinkAuthorityKeyCodec.actor("member"),
                                () -> true,
                                snapshot ->
                                        queue.enqueueLifecycleBarrier(
                                                () -> {
                                                    actorApplied.incrementAndGet();
                                                    return CompletableFuture.completedFuture(null);
                                                }),
                                () -> parentKey,
                                disposed::incrementAndGet);
                await(member.ready());
                queue.executionPolicy(member);
                var held = new CompletableFuture<ZLinkAuthorityWriteResult>();
                f.heldWrites.put(parentKey, held);
                f.outage();
                f.at(35, 0);
                var renewal = f.runtime.renewOwnerLeaseOnce();
                var dispatched = queue.enqueue(() -> CompletableFuture.completedFuture(null), null);
                assertTrue(member.paused());
                assertEquals(0, actorApplied.get());
                assertFalse(dispatched.toCompletableFuture().isDone());
                held.complete(f.awaitSubmitted(parentKey));
                await(renewal);
                await(dispatched);
                assertFalse(parent.paused());
                assertFalse(member.paused());
                assertEquals(1, spotApplied.get());
                assertEquals(1, actorApplied.get());
                assertEquals(0, disposed.get());
                queue.close();
            }
        }
    }

    @RepeatedTest(100)
    void yieldedContinuationWaitsOnTheSameQueueUntilItsRecoveryIsConfirmed() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new RuntimeFixture(provider)) {
                var disposed = new java.util.concurrent.atomic.AtomicInteger();
                var applied = new java.util.concurrent.atomic.AtomicInteger();
                var queue =
                        new systems.zlink.framework.execution.ZLinkSerialExecutionQueue(
                                Runnable::run,
                                systems.zlink.framework.execution.ZLinkExecutionLanePolicy.spot());
                f.f.spot("a");
                var a =
                        f.runtime.execution(
                                ZLinkAuthorityKeyCodec.spot("a"),
                                () -> true,
                                snapshot ->
                                        queue.enqueueLifecycleBarrier(
                                                () -> {
                                                    applied.incrementAndGet();
                                                    return CompletableFuture.completedFuture(null);
                                                }),
                                () -> null,
                                disposed::incrementAndGet);
                await(a.ready());
                queue.executionPolicy(a);
                var remote = new CompletableFuture<Void>();
                var yielded = new CompletableFuture<Void>();
                var calls = new java.util.concurrent.atomic.AtomicInteger();
                var first =
                        queue.enqueue(
                                () -> {
                                    var continuation =
                                            systems.zlink.framework.execution
                                                    .ZLinkSerialExecutionQueue.yieldCurrent(remote);
                                    yielded.complete(null);
                                    return continuation.thenRun(calls::incrementAndGet);
                                },
                                null);
                await(yielded);
                f.outage();
                f.at(35, 0);
                f.publication = new CompletableFuture<>();
                var renewal = f.runtime.renewOwnerLeaseOnce();
                remote.complete(null);
                await(queue.enqueueLifecycleBarrier(() -> CompletableFuture.completedFuture(null)));
                assertEquals(0, calls.get());
                assertFalse(first.toCompletableFuture().isDone());
                f.publication.complete(null);
                await(renewal);
                await(first);
                assertEquals(1, calls.get());
                assertEquals(1, applied.get());
                assertEquals(0, disposed.get());
                queue.close();
            }
        }
    }

    @RepeatedTest(100)
    void aDifferentMeshPublicationCannotDelayOrRevokeAConfirmedObject() {
        for (boolean provider : new boolean[] {false, true}) {
            for (boolean rejected : new boolean[] {false, true}) {
                try (var f = new RuntimeFixture(provider)) {
                    var disposedA = new java.util.concurrent.atomic.AtomicInteger();
                    var disposedB = new java.util.concurrent.atomic.AtomicInteger();
                    var appliedA = new java.util.concurrent.atomic.AtomicInteger();
                    var appliedB = new java.util.concurrent.atomic.AtomicInteger();
                    var firstNode = f.f.node;
                    var a = f.bind("a", disposedA, appliedA);
                    f.f.node =
                            new ZLinkMeshNodeDescriptorKey("other", RoutingId.from("other-node"));
                    var secondNode = f.f.node;
                    f.f.publishedLifecycle = 0;
                    f.f.publish(1);
                    var b = f.bind("b", disposedB, appliedB);
                    var held = new CompletableFuture<Void>();
                    f.runtime.setOwnerLeaseRecoveryListener(
                            open -> {
                                f.countPublication();
                                f.f.owner = f.runtime.currentOwnerToken();
                                var first =
                                        f.runtime.writeDescriptor(
                                                firstNode,
                                                () -> {
                                                    f.f.node = firstNode;
                                                    f.f.publish(1);
                                                    return held.thenApply(
                                                                    done ->
                                                                            rejected
                                                                                    ? ZLinkLocationWriteResult
                                                                                            .rejectedConflict()
                                                                                    : ZLinkLocationWriteResult
                                                                                            .stored(
                                                                                                    1,
                                                                                                    f
                                                                                                            .f
                                                                                                            .clock
                                                                                                            .instant()))
                                                            .thenApply(
                                                                    RuntimeFixture
                                                                            ::confirmPublication);
                                                });
                                var second =
                                        f.runtime.writeDescriptor(
                                                secondNode,
                                                () -> {
                                                    f.f.node = secondNode;
                                                    f.f.publish(1);
                                                    return CompletableFuture.completedFuture(
                                                            ZLinkLocationWriteResult.stored(
                                                                    1, f.f.clock.instant()));
                                                });
                                return CompletableFuture.allOf(
                                        first.toCompletableFuture(), second.toCompletableFuture());
                            });
                    f.outage();
                    f.at(35, 0);
                    var recovery = f.runtime.renewOwnerLeaseOnce();
                    f.awaitWrites(2);
                    assertEquals(2, f.writes);
                    assertEquals(1, appliedB.get());
                    assertEquals(0, appliedA.get());
                    assertTrue(a.paused());
                    assertFalse(b.paused());
                    b.check(true);
                    held.complete(null);
                    await(recovery);
                    assertEquals(rejected ? 0 : 1, appliedA.get());
                    assertEquals(rejected ? 1 : 0, disposedA.get());
                    assertEquals(0, disposedB.get());
                    b.check(true);
                    int calls = f.clockReads;
                    b.check(true);
                    assertEquals(
                            calls,
                            f.clockReads,
                            "finishing another mesh must preserve the normal fast path");
                }
            }
        }
    }

    @RepeatedTest(100)
    void providerReadFailureKeepsTheOriginalRecordForTheNextRenewal() {
        for (boolean provider : new boolean[] {false, true}) {
            for (boolean synchronous : new boolean[] {false, true}) {
                try (var f = new RuntimeFixture(provider)) {
                    var disposed = new java.util.concurrent.atomic.AtomicInteger();
                    var applied = new java.util.concurrent.atomic.AtomicInteger();
                    var a = f.bind("a", disposed, applied);
                    String key = ZLinkAuthorityKeyCodec.spot("a");
                    f.outage();
                    f.at(35, 0);
                    f.synchronousReadFailure = synchronous;
                    if (!synchronous) {
                        var failed = new CompletableFuture<ZLinkAuthorityReadResult>();
                        failed.completeExceptionally(
                                new IllegalStateException("provider read failed"));
                        f.heldReads.put(key, failed);
                    }
                    await(f.runtime.renewOwnerLeaseOnce());
                    assertEquals(0, disposed.get());
                    assertEquals(0, applied.get());
                    assertEquals(0, f.writes);
                    assertTrue(a.paused());
                    f.synchronousReadFailure = false;
                    f.heldReads.clear();
                    await(f.runtime.renewOwnerLeaseOnce());
                    assertEquals(0, disposed.get());
                    assertEquals(1, applied.get());
                    assertEquals(1, f.writes);
                    assertFalse(a.paused());
                }
            }
        }
    }

    @RepeatedTest(100)
    void cleanupFailureDoesNotPreventAnotherOwnerFromResumingOrTheNextHeartbeat() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new RuntimeFixture(provider)) {
                var disposed = new java.util.concurrent.atomic.AtomicInteger();
                var appliedA = new java.util.concurrent.atomic.AtomicInteger();
                var appliedB = new java.util.concurrent.atomic.AtomicInteger();
                f.bind("a", disposed, appliedA);
                var b = f.bind("b", disposed, appliedB);
                var key = ZLinkAuthorityKeyCodec.spot("a");
                var original = f.f.read(key);
                await(
                        f.f.repository.compareExchange(
                                key,
                                new ZLinkAuthorityExpectFound(original.storeVersion()),
                                new ZLinkAuthorityPut(original.payload()),
                                () -> false));
                f.cleanupError = true;
                f.outage();
                f.at(35, 0);
                assertFalse(await(f.runtime.renewOwnerLeaseOnce()));
                assertEquals(1, disposed.get());
                assertEquals(1, appliedB.get());
                b.check(true);
                int attempts = f.cleanups;
                f.cleanupError = false;
                assertTrue(await(f.runtime.renewOwnerLeaseOnce()));
                assertEquals(attempts + 1, f.cleanups, "one cleanup attempt per heartbeat");
                assertEquals(1, appliedB.get());
                assertEquals(0, appliedA.get());
                b.check(true);
            }
        }
    }

    @RepeatedTest(100)
    void runtimeCloseKeepsQueuedApplicationShutdownResult() {
        assertAll(
                () -> runtimeCloseKeepsQueuedApplicationShutdownResult(false, false),
                () -> runtimeCloseKeepsQueuedApplicationShutdownResult(false, true),
                () -> runtimeCloseKeepsQueuedApplicationShutdownResult(true, false),
                () -> runtimeCloseKeepsQueuedApplicationShutdownResult(true, true));
    }

    private void runtimeCloseKeepsQueuedApplicationShutdownResult(
            boolean provider, boolean recovering) {
        try (var f = new RuntimeFixture(provider)) {
            var disposed = new java.util.concurrent.atomic.AtomicInteger();
            var calls = new java.util.concurrent.atomic.AtomicInteger();
            var queue =
                    new systems.zlink.framework.execution.ZLinkSerialExecutionQueue(
                            Runnable::run,
                            systems.zlink.framework.execution.ZLinkExecutionLanePolicy.spot());
            f.f.spot("a");
            var a =
                    f.runtime.execution(
                            ZLinkAuthorityKeyCodec.spot("a"),
                            () -> true,
                            snapshot -> CompletableFuture.completedFuture(null),
                            () -> null,
                            () -> {
                                disposed.incrementAndGet();
                                queue.discardApplications();
                            });
            await(a.ready());
            queue.executionPolicy(a);
            var active = new CompletableFuture<Void>();
            var entered = new CompletableFuture<Void>();
            queue.enqueue(
                    () -> {
                        entered.complete(null);
                        return active;
                    },
                    null);
            await(entered);
            f.outage();
            if (recovering) {
                f.at(35, 0);
                f.publication = new CompletableFuture<>();
                f.runtime.renewOwnerLeaseOnce();
            }
            var pending =
                    queue.enqueue(
                            () -> {
                                calls.incrementAndGet();
                                return CompletableFuture.completedFuture(null);
                            },
                            null);
            assertFalse(pending.toCompletableFuture().isDone());
            f.runtime.close();
            active.complete(null);
            var failure = assertThrows(CompletionException.class, () -> await(pending));
            assertEquals(
                    systems.zlink.framework.errors.ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                    assertInstanceOf(
                                    systems.zlink.framework.errors.ZLinkFrameworkException.class,
                                    failure.getCause())
                            .kind());
            assertEquals(0, calls.get());
            assertEquals(0, disposed.get(), "host shutdown owns remaining object cleanup");
            f.publication.complete(null);
            queue.close();
        }
    }

    @RepeatedTest(100)
    void closeDuringRecoveryCannotApplyALateAuthorityOrReopenTheGate() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new RuntimeFixture(provider)) {
                var disposed = new java.util.concurrent.atomic.AtomicInteger();
                var applied = new java.util.concurrent.atomic.AtomicInteger();
                var a = f.bind("a", disposed, applied);
                String key = ZLinkAuthorityKeyCodec.spot("a");
                var held = new CompletableFuture<ZLinkAuthorityWriteResult>();
                f.heldWrites.put(key, held);
                f.outage();
                f.at(35, 0);
                var recovery = f.runtime.renewOwnerLeaseOnce();
                f.runtime.close();
                held.complete(f.awaitSubmitted(key));
                await(recovery);
                assertEquals(1, disposed.get());
                assertEquals(0, applied.get());
                var failure =
                        assertThrows(
                                systems.zlink.framework.errors.ZLinkFrameworkException.class,
                                () -> a.check(true));
                assertEquals(
                        systems.zlink.framework.errors.ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                        failure.kind());
            }
        }
    }

    @RepeatedTest(100)
    void incarnationCloseDuringRecoveryCannotApplyOrRetainALateAuthority() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new RuntimeFixture(provider)) {
                var disposed = new java.util.concurrent.atomic.AtomicInteger();
                var applied = new java.util.concurrent.atomic.AtomicInteger();
                var a = f.bind("a", disposed, applied);
                String key = ZLinkAuthorityKeyCodec.spot("a");
                var held = new CompletableFuture<ZLinkAuthorityWriteResult>();
                f.heldWrites.put(key, held);
                f.outage();
                f.at(35, 0);
                var recovery = f.runtime.renewOwnerLeaseOnce();
                a.close();
                held.complete(f.awaitSubmitted(key));
                await(recovery);
                assertEquals(0, applied.get());
                assertEquals(1, disposed.get());
                assertInstanceOf(
                        ZLinkAuthorityMissing.class, await(f.f.repository.read(key, () -> false)));
                assertThrows(
                        systems.zlink.framework.errors.ZLinkFrameworkException.class,
                        () -> a.check(true));
            }
        }
    }

    @RepeatedTest(100)
    void failedConcurrentRenewalCannotBecomeTheNextRetentionBasis() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new RuntimeFixture(provider)) {
                f.bind(
                        "a",
                        new java.util.concurrent.atomic.AtomicInteger(),
                        new java.util.concurrent.atomic.AtomicInteger());
                f.outage();
                f.at(35, 0);
                f.publication = new CompletableFuture<>();
                var recovery = f.runtime.renewOwnerLeaseOnce();
                f.at(36, 0);
                f.unavailable = true;
                assertFalse(await(f.runtime.renewOwnerLeaseOnce()));
                f.unavailable = false;
                f.publication.complete(null);
                await(recovery);
                assertEquals(
                        Duration.ofSeconds(35).toNanos(),
                        f.runtime.ownerLeaseSnapshot().lastSuccessfulRequestStartedNanos());
                f.outage();
                assertEquals(
                        Duration.ofSeconds(75).toNanos(),
                        f.runtime.ownerLeaseSnapshot().outageCutoffNanos());
            }
        }
    }

    @RepeatedTest(100)
    void theMaterializationOwnerRegistersAuthorityAtTheReadyBoundary() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new RuntimeFixture(provider)) {
                var lifecycle = new ZLinkLocationLifecycle(f.runtime);
                var disposed = new java.util.concurrent.atomic.AtomicInteger();
                var applied = new java.util.concurrent.atomic.AtomicInteger();
                var actorId = "registered";
                var key = ZLinkAuthorityKeyCodec.actor(actorId);
                var payload =
                        new ZLinkActorAuthorityPayloadCodec()
                                .encode(
                                        ZLinkActorAuthorityPayloadCodec.State.READY,
                                        "player",
                                        actorId,
                                        "entry-game",
                                        1,
                                        systems.zlink.framework.spots.ZLinkSpotKind.ENTRY.value(),
                                        f.f.owner.ownerId(),
                                        f.f.owner.leaseGeneration(),
                                        f.f.node.meshName(),
                                        f.f.node.rid(),
                                        1);
                var reservation = f.f.reserve(key, true, payload);
                assertEquals(
                        ZLinkObjectCommitResult.COMMITTED,
                        await(f.f.repository.commit(reservation, payload, () -> false)));
                var execution =
                        lifecycle.execution(
                                actorId,
                                () -> {
                                    f.collected++;
                                    return true;
                                },
                                snapshot -> {
                                    applied.incrementAndGet();
                                    return CompletableFuture.completedFuture(null);
                                },
                                () -> null,
                                disposed::incrementAndGet);
                await(execution.ready());
                assertEquals(0, f.reads, "a local materialization has no authority registration");
                await(
                        lifecycle.setActorRef(
                                "player",
                                actorId,
                                new systems.zlink.framework.actors.ActorRef(
                                        actorId,
                                        reservation.objectGeneration(),
                                        f.f.node.meshName(),
                                        f.f.node.rid())));
                await(execution.ready());
                assertEquals(
                        1,
                        f.reads,
                        "the existing materialization owner supplies the authority key");
                f.outage();
                f.at(35, 0);
                await(f.runtime.renewOwnerLeaseOnce());
                assertEquals(1, f.collected, "Ready updates do not duplicate an execution record");
                assertEquals(1, f.writes);
                assertEquals(1, applied.get());
                assertEquals(0, disposed.get());
                assertFalse(execution.paused());
                execution.check(true);
            }
        }
    }

    @RepeatedTest(100)
    void unconfirmedDescriptorContinuesOnTheExistingRenewalPath() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new RuntimeFixture(provider)) {
                var disposed = new java.util.concurrent.atomic.AtomicInteger();
                var applied = new java.util.concurrent.atomic.AtomicInteger();
                var a = f.bind("a", disposed, applied);
                var attempts = new java.util.concurrent.atomic.AtomicInteger();
                f.runtime.setOwnerLeaseRecoveryListener(
                        open -> {
                            if (open) return CompletableFuture.completedFuture(null);
                            int attempt = attempts.incrementAndGet();
                            f.countPublication();
                            f.f.owner = f.runtime.currentOwnerToken();
                            return f.runtime
                                    .writeDescriptor(
                                            f.f.node,
                                            () -> {
                                                f.f.publish(1);
                                                return attempt == 1
                                                        ? CompletableFuture.failedFuture(
                                                                new TimeoutException(
                                                                        "descriptor confirmation lost"))
                                                        : CompletableFuture.completedFuture(
                                                                ZLinkLocationWriteResult.stored(
                                                                        1, f.f.clock.instant()));
                                            })
                                    .thenApply(ignored -> null);
                        });
                f.outage();
                f.at(35, 0);
                await(f.runtime.renewOwnerLeaseOnce());
                assertTrue(a.paused());
                assertEquals(0, applied.get());
                assertEquals(1, f.writes);
                assertEquals(1, attempts.get());
                f.at(35, 1);
                await(f.runtime.renewOwnerLeaseOnce());
                assertEquals(
                        2,
                        attempts.get(),
                        "new host admission cannot suppress unfinished descriptor recovery");
                assertEquals(
                        1, f.writes, "exact authority confirmation never repeats the applied CAS");
                assertFalse(a.paused());
                assertEquals(1, applied.get());
                assertEquals(0, disposed.get());
            }
        }
    }

    @RepeatedTest(100)
    void lateIndeterminatePreserveCanStillBeConfirmedBeforeTheOriginalCutoff() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new RuntimeFixture(provider)) {
                var disposed = new java.util.concurrent.atomic.AtomicInteger();
                var applied = new java.util.concurrent.atomic.AtomicInteger();
                var a = f.bind("a", disposed, applied);
                String key = ZLinkAuthorityKeyCodec.spot("a");
                var network = new CompletableFuture<Void>();
                f.beforeWrites.put(key, network);
                var applicationObserved = new CompletableFuture<Void>();
                f.afterWrite = () -> applicationObserved.complete(null);
                f.outage();
                f.at(35, 0);
                var first = f.runtime.renewOwnerLeaseOnce();
                f.awaitWrites(1);
                assertEquals(1, f.writes);
                f.at(38, 0);
                for (var task : List.copyOf(f.scheduler.tasks)) {
                    if (!task.isCancelled()
                            && task.getDelay(TimeUnit.NANOSECONDS)
                                    == Duration.ofSeconds(3).toNanos()) task.run();
                }
                await(first);
                f.at(38, 1);
                await(f.runtime.renewOwnerLeaseOnce());
                assertEquals(1, f.writes, "insufficient time never submits another CAS");
                assertEquals(
                        0,
                        disposed.get(),
                        "submission budget does not replace the original T latch");
                assertTrue(a.paused());
                f.at(39, 0);
                network.complete(null);
                await(applicationObserved);
                assertInstanceOf(ZLinkAuthorityStored.class, f.awaitSubmitted(key));
                f.at(39, 1);
                await(f.runtime.renewOwnerLeaseOnce());
                assertEquals(1, f.writes, "late application is confirmed by exact read only");
                assertEquals(0, disposed.get());
                assertEquals(1, applied.get());
                assertFalse(a.paused());
            }
        }
    }

    @RepeatedTest(100)
    void aRecoveredOwnerKeepsTheFastPathWhenAnotherOwnerExpiresAtT() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new RuntimeFixture(provider)) {
                var disposedA = new java.util.concurrent.atomic.AtomicInteger();
                var disposedB = new java.util.concurrent.atomic.AtomicInteger();
                var a = f.bind("a", disposedA, new java.util.concurrent.atomic.AtomicInteger());
                var b = f.bind("b", disposedB, new java.util.concurrent.atomic.AtomicInteger());
                String key = ZLinkAuthorityKeyCodec.spot("a");
                var held = new CompletableFuture<ZLinkAuthorityWriteResult>();
                f.heldWrites.put(key, held);
                f.outage();
                f.at(35, 0);
                var recovery = f.runtime.renewOwnerLeaseOnce();
                assertTrue(a.paused());
                assertFalse(b.paused());
                f.at(40, 0);
                for (var task : List.copyOf(f.scheduler.tasks)) {
                    if (!task.isCancelled()
                            && task.getDelay(TimeUnit.NANOSECONDS)
                                    > Duration.ofSeconds(5).toNanos()) task.run();
                }
                assertEquals(1, disposedA.get());
                assertEquals(0, disposedB.get());
                int reads = f.clockReads;
                b.check(true);
                assertEquals(
                        reads,
                        f.clockReads,
                        "a discarded peer cannot add a clock read to the recovered owner");
                held.complete(f.awaitSubmitted(key));
                await(recovery);
                f.at(41, 0);
                await(f.runtime.renewOwnerLeaseOnce());
                reads = f.clockReads;
                b.check(true);
                assertEquals(reads, f.clockReads);
                assertThrows(
                        systems.zlink.framework.errors.ZLinkFrameworkException.class,
                        () -> a.check(true));
                assertEquals(1, disposedA.get());
                assertEquals(0, disposedB.get());
            }
        }
    }
}
