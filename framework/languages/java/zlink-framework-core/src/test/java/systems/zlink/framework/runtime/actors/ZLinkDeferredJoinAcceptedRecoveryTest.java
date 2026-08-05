package systems.zlink.framework.runtime.actors;

import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationStore;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationDeleteResult;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationFound;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationMissing;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationReadResult;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationRenewMissing;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationRenewResult;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationRenewed;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationStored;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.time.Duration;
import java.time.Instant;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.actors.ZLinkActorJoinOperationId;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

final class ZLinkDeferredJoinAcceptedRecoveryTest {
    @Test
    void relocationRootPreservesOperationReplyCursorAndGenerationAcrossRetry() {
        MemoryRelocationStore store = new MemoryRelocationStore();
        ZLinkDeferredJoinAcceptedRecovery recovery =
            new ZLinkDeferredJoinAcceptedRecovery(
                store,
                new ZLinkJsonMessageSerializer());
        ZLinkActorJoinOperationId operation =
            new ZLinkActorJoinOperationId(41, 73);
        ZLinkBackendActorRef actorRef = new ZLinkBackendActorRef(
            RoutingId.from("target-node"),
            "actor-a",
            17);
        ZLinkDeferredJoinAcceptedRecovery.Manifest manifest = recovery.prepare(
                operation,
                actorRef,
                "\"accepted\"".getBytes(java.nio.charset.StandardCharsets.UTF_8))
            .toCompletableFuture()
            .join();
        RecordingActor actor = new RecordingActor("actor-a");
        AtomicInteger mailboxAdmissions = new AtomicInteger();

        assertThrows(
            CompletionException.class,
            () -> recovery.deliver(
                    manifest,
                    actorRef,
                    "game",
                    ignored -> actor,
                    (actorId, callback) -> {
                        mailboxAdmissions.incrementAndGet();
                        return callback.get();
                    })
                .toCompletableFuture()
                .join());

        recovery.deliver(
                manifest,
                actorRef,
                "game",
                ignored -> actor,
                (actorId, callback) -> {
                    mailboxAdmissions.incrementAndGet();
                    return callback.get();
                })
            .toCompletableFuture()
            .join();
        recovery.deliver(
                manifest,
                actorRef,
                "game",
                ignored -> actor,
                (actorId, callback) -> {
                    mailboxAdmissions.incrementAndGet();
                    return callback.get();
                })
            .toCompletableFuture()
            .join();

        assertEquals(2, mailboxAdmissions.get());
        assertEquals(2, actor.attempts.get());
        assertEquals(operation, actor.accepted.operationId());
        assertEquals(17, actor.accepted.actor().objectGeneration());
        assertEquals("accepted", actor.accepted.reply().decode(String.class));
        assertEquals(1, manifest.cursor());
    }

    @Test
    void generationFenceRejectsAReplacementActorBeforeMailboxAdmission() {
        MemoryRelocationStore store = new MemoryRelocationStore();
        ZLinkDeferredJoinAcceptedRecovery recovery =
            new ZLinkDeferredJoinAcceptedRecovery(
                store,
                new ZLinkJsonMessageSerializer());
        ZLinkDeferredJoinAcceptedRecovery.Manifest manifest = recovery.prepare(
                new ZLinkActorJoinOperationId(1, 2),
                new ZLinkBackendActorRef(
                    RoutingId.from("source-node"),
                    "actor-a",
                    9),
                new byte[0])
            .toCompletableFuture()
            .join();

        assertThrows(
            CompletionException.class,
            () -> recovery.deliver(
                    manifest,
                    new ZLinkBackendActorRef(
                        RoutingId.from("target-node"),
                        "actor-a",
                        10),
                    "game",
                    ignored -> new RecordingActor("actor-a"),
                    (actorId, callback) -> callback.get())
                .toCompletableFuture()
                .join());
    }

    private static final class RecordingActor implements ZLinkActor {
        private final String actorId;
        private final AtomicInteger attempts = new AtomicInteger();
        private ZLinkActorJoinCompletion.Accepted accepted;

        private RecordingActor(String actorId) {
            this.actorId = actorId;
        }

        @Override
        public ZLinkActorContext context() {
            return null;
        }

        @Override
        public CompletionStage<Void> onJoinCompleted(
            ZLinkActorJoinCompletion completion) {
            if (attempts.getAndIncrement() == 0) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException("retry"));
            }
            accepted = (ZLinkActorJoinCompletion.Accepted) completion;
            return CompletableFuture.completedFuture(null);
        }
    }

    private static final class MemoryRelocationStore
        implements ZLinkRelocationStore {
        private final Map<String, byte[]> roots = new ConcurrentHashMap<>();
        private final AtomicInteger sequence = new AtomicInteger();

        @Override
        public CompletionStage<ZLinkRelocationStored> put(
            byte[] payload,
            Duration retention,
            ZLinkStoreCancellation cancellation) {
            String reference = "root-" + sequence.incrementAndGet();
            roots.put(reference, payload.clone());
            Instant now = Instant.now();
            java.util.zip.CRC32C checksum = new java.util.zip.CRC32C();
            checksum.update(payload, 0, payload.length);
            return CompletableFuture.completedFuture(new ZLinkRelocationStored(
                reference,
                checksum.getValue(),
                now.plus(retention),
                now));
        }

        @Override
        public CompletionStage<ZLinkRelocationReadResult> get(
            String reference,
            ZLinkStoreCancellation cancellation) {
            byte[] payload = roots.get(reference);
            return CompletableFuture.completedFuture(payload == null
                ? new ZLinkRelocationMissing()
                : new ZLinkRelocationFound(payload));
        }

        @Override
        public CompletionStage<ZLinkRelocationRenewResult> renew(
            String reference,
            Duration retention,
            ZLinkStoreCancellation cancellation) {
            return CompletableFuture.completedFuture(
                roots.containsKey(reference)
                    ? new ZLinkRelocationRenewed(
                        Instant.now().plus(retention),
                        Instant.now())
                    : new ZLinkRelocationRenewMissing());
        }

        @Override
        public CompletionStage<ZLinkRelocationDeleteResult> delete(
            String reference,
            ZLinkStoreCancellation cancellation) {
            return CompletableFuture.completedFuture(
                roots.remove(reference) == null
                    ? ZLinkRelocationDeleteResult.MISSING
                    : ZLinkRelocationDeleteResult.DELETED);
        }
    }
}
