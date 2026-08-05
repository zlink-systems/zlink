package systems.zlink.framework.runtime.actors;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.time.Duration;
import java.util.Objects;
import java.util.Set;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.BiFunction;
import java.util.function.Function;
import java.util.function.Supplier;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.actors.ZLinkActorJoinOperationId;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationStore;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkDeferredJoinCompletionAuthority;

/**
 * Stores and replays the cross-node Accepted completion as part of the
 * relocation operation rather than relying on the source process callback.
 */
final class ZLinkDeferredJoinAcceptedRecovery {
    private static final boolean STREAM_TRACE =
        "1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"));
    private static final java.util.logging.Logger LOGGER =
        java.util.logging.Logger.getLogger(ZLinkDeferredJoinAcceptedRecovery.class.getName());
    private static final int FORMAT_VERSION = 1;
    private static final int CURSOR_COMMITTED = 1;
    private static final Duration RETENTION = Duration.ofHours(24);
    private static final ZLinkStoreCancellation NOT_CANCELLED = () -> false;

    private final ZLinkRelocationStore store;
    private final ZLinkMessageSerializer serializer;
    private final ZLinkDeferredJoinCompletionAuthority authorityJournal;
    private final Set<ZLinkActorJoinOperationId> delivered =
        ConcurrentHashMap.newKeySet();

    ZLinkDeferredJoinAcceptedRecovery(
        ZLinkRelocationStore store,
        ZLinkMessageSerializer serializer) {
        this.store = Objects.requireNonNull(store, "store");
        this.serializer = Objects.requireNonNull(serializer, "serializer");
        this.authorityJournal = null;
    }

    ZLinkDeferredJoinAcceptedRecovery(
        ZLinkLocationRepository authority,
        ZLinkRelocationStore store,
        ZLinkMessageSerializer serializer) {
        this.store = Objects.requireNonNull(store, "store");
        this.serializer = Objects.requireNonNull(serializer, "serializer");
        this.authorityJournal = new ZLinkDeferredJoinCompletionAuthority(
            Objects.requireNonNull(authority, "authority"),
            store);
    }

    CompletionStage<Manifest> prepare(
        ZLinkActorJoinOperationId operationId,
        ZLinkBackendActorRef actor,
        byte[] rawReply) {
        if (authorityJournal != null) {
            return authorityJournal.prepare(operationId, actor, rawReply)
                .thenApply(value -> new Manifest(
                    value.reference(),
                    value.checksumCrc32c(),
                    value.cursor(),
                    operationId.high(),
                    operationId.low(),
                    actor.actorId(),
                    actor.generation()));
        }
        return prepareLegacy(operationId, actor, rawReply);
    }

    CompletionStage<Manifest> prepareRelocation(
        UUID relocationId,
        ZLinkActorJoinOperationId operationId,
        ZLinkBackendActorRef actor,
        String actorType,
        String targetSpotId,
        systems.zlink.contracts.core.RoutingId targetNodeRid,
        boolean restoreSnapshot,
        byte[] applicationState,
        List<ZLinkAsyncSerialQueue.QueuedRecord> acceptedJournal,
        byte[] rawReply,
        byte[] sessionRouteCommand44) {
        if (authorityJournal == null) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "direct Actor Join requires canonical Location authority"));
        }
        return authorityJournal.prepareRelocation(
                relocationId,
                operationId,
                actor,
                actorType,
                targetSpotId,
                targetNodeRid,
                restoreSnapshot,
                applicationState,
                acceptedJournal,
                rawReply,
                sessionRouteCommand44)
            .thenApply(value -> new Manifest(
                value.published().reference(),
                value.published().checksumCrc32c(),
                value.published().cursor(),
                operationId.high(),
                operationId.low(),
                actor.actorId(),
                actor.generation(),
                value.fence().aggregateId().getMostSignificantBits(),
                value.fence().aggregateId().getLeastSignificantBits(),
                value.fence().aggregateGeneration()));
    }

    CompletionStage<Long> commitPrepared(
        Manifest manifest,
        ZLinkBackendActorRef actor) {
        if (authorityJournal == null || !manifest.hasAggregateFence()) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "direct Actor Join relocation fence is missing"));
        }
        return authorityJournal.commitPrepared(
            new UUID(
                manifest.aggregateIdHigh(),
                manifest.aggregateIdLow()),
            manifest.aggregateGeneration(),
            actor);
    }

    CompletionStage<Void> awaitTargetCommit(
        Manifest manifest,
        ZLinkBackendActorRef actor,
        Duration timeout) {
        if (authorityJournal == null || !manifest.hasAggregateFence()) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "direct Actor Join relocation fence is missing"));
        }
        return authorityJournal.awaitTargetCommit(
            new UUID(
                manifest.aggregateIdHigh(),
                manifest.aggregateIdLow()),
            manifest.aggregateGeneration(),
            actor,
            timeout);
    }

    CompletionStage<Void> awaitTargetCompletion(
        Manifest manifest,
        ZLinkBackendActorRef actor,
        Duration timeout) {
        if (authorityJournal == null || !manifest.hasAggregateFence()) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "direct Actor Join relocation fence is missing"));
        }
        return authorityJournal.awaitTargetCompletion(
            new UUID(
                manifest.aggregateIdHigh(),
                manifest.aggregateIdLow()),
            manifest.aggregateGeneration(),
            new ZLinkActorJoinOperationId(
                manifest.operationIdHigh(),
                manifest.operationIdLow()),
            actor,
            timeout);
    }

    CompletionStage<Void> awaitSourceCleanup(
        Manifest manifest,
        ZLinkBackendActorRef actor,
        Duration timeout) {
        if (authorityJournal == null || !manifest.hasAggregateFence()) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "direct Actor Join relocation fence is missing"));
        }
        return authorityJournal.awaitSourceCleanup(
            new UUID(
                manifest.aggregateIdHigh(),
                manifest.aggregateIdLow()),
            manifest.aggregateGeneration(),
            actor,
            timeout);
    }

    CompletionStage<PreparedRoot> loadPrepared(
        Manifest manifest,
        ZLinkBackendActorRef actor,
        boolean restoreSnapshot) {
        if (authorityJournal == null || !manifest.hasAggregateFence()) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "direct Actor Join relocation fence is missing"));
        }
        return authorityJournal.loadPrepared(
                manifest.reference(),
                manifest.checksumCrc32c(),
                new ZLinkActorJoinOperationId(
                    manifest.operationIdHigh(),
                    manifest.operationIdLow()),
                actor,
                new UUID(
                    manifest.aggregateIdHigh(),
                    manifest.aggregateIdLow()),
                restoreSnapshot)
            .thenApply(root -> new PreparedRoot(
                root.applicationState(),
                root.acceptedJournal(),
                root.sessionRouteCommand44()));
    }

    CompletionStage<Void> abortPrepared(Manifest manifest) {
        if (authorityJournal == null || !manifest.hasAggregateFence()) {
            return CompletableFuture.completedFuture(null);
        }
        return authorityJournal.abortPrepared(
            new UUID(
                manifest.aggregateIdHigh(),
                manifest.aggregateIdLow()),
            manifest.aggregateGeneration(),
            manifest.reference());
    }

    private CompletionStage<Manifest> prepareLegacy(
        ZLinkActorJoinOperationId operationId,
        ZLinkBackendActorRef actor,
        byte[] rawReply) {
        byte[] payload = encode(operationId, actor, rawReply);
        return store.put(payload, RETENTION, NOT_CANCELLED)
            .thenApply(stored -> new Manifest(
                stored.reference(),
                stored.checksumCrc32c(),
                CURSOR_COMMITTED));
    }

    CompletionStage<Void> deliver(
        Manifest manifest,
        ZLinkBackendActorRef currentActor,
        ZLinkActorRuntime runtime) {
        return deliver(
            manifest,
            currentActor,
            runtime.meshName(),
            runtime::actorById,
            runtime::submitActorDispatch);
    }

    CompletionStage<Void> deliver(
        Manifest manifest,
        ZLinkBackendActorRef currentActor,
        String meshName,
        Function<String, ZLinkActor> actorResolver,
        BiFunction<String, Supplier<CompletionStage<Void>>, CompletionStage<Void>>
            mailbox) {
        if (authorityJournal != null && !manifest.actorId().isEmpty()) {
            return deliverCanonical(
                manifest,
                currentActor,
                meshName,
                actorResolver,
                mailbox);
        }
        return store.get(manifest.reference(), NOT_CANCELLED)
            .thenCompose(result -> {
                if (!(result instanceof ZLinkRelocationFound found)) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "deferred Actor Join completion root is missing"));
                }
                if (crc32c(found.payload()) != manifest.checksumCrc32c()) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "deferred Actor Join completion checksum mismatch"));
                }
                Stored stored = decode(found.payload());
                if (!stored.actorId().equals(currentActor.actorId())
                    || stored.objectGeneration() != currentActor.generation()) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "deferred Actor Join completion generation fence is stale"));
                }
                if (!delivered.add(stored.operationId())) {
                    return CompletableFuture.completedFuture(null);
                }
                ZLinkActor actor = actorResolver.apply(stored.actorId());
                if (actor == null) {
                    delivered.remove(stored.operationId());
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "target Actor is not materialized for Join completion"));
                }
                ActorRef publicRef = ZLinkActorRuntime.toPublicActorRef(
                    currentActor,
                    meshName);
                ZLinkMessage reply = stored.rawReply().length == 0
                    ? ZLinkMessage.empty()
                    : ZLinkMessage.fromEncoded(
                        ZLinkEncodedPayload.from(stored.rawReply()),
                        serializer);
                return mailbox.apply(
                        stored.actorId(),
                        () -> {
                            CompletionStage<Void> callback =
                                actor.onJoinCompleted(
                                    new ZLinkActorJoinCompletion.Accepted(
                                        stored.operationId(),
                                        publicRef,
                                        reply));
                            return callback == null
                                ? CompletableFuture.completedFuture(null)
                                : callback;
                        })
                    .whenComplete((ignored, error) -> {
                        if (error != null) {
                            // A retry re-enters the Actor mailbox with the same
                            // OperationId. Applications use it as the side-effect
                            // idempotency key.
                            delivered.remove(stored.operationId());
                        }
                    });
            });
    }

    private static boolean isCanonicalRootMissing(Throwable error) {
        Throwable current = error;
        while (current instanceof java.util.concurrent.CompletionException
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current instanceof ZLinkDeferredJoinCompletionAuthority
            .CanonicalRootMissingException;
    }

    private CompletionStage<Void> deliverCanonical(
        Manifest manifest,
        ZLinkBackendActorRef currentActor,
        String meshName,
        Function<String, ZLinkActor> actorResolver,
        BiFunction<String, Supplier<CompletionStage<Void>>, CompletionStage<Void>>
            mailbox) {
        ZLinkActorJoinOperationId operationId =
            new ZLinkActorJoinOperationId(
                manifest.operationIdHigh(),
                manifest.operationIdLow());
        return authorityJournal.restore(
                manifest.reference(),
                manifest.checksumCrc32c(),
                operationId,
                currentActor)
            .exceptionallyCompose(error ->
                isCanonicalRootMissing(error)
                    ? authorityJournal.recoverSuccessor(
                        manifest.reference(),
                        operationId,
                        currentActor)
                    : CompletableFuture.failedFuture(error))
            .thenCompose(restored -> authorityJournal.advance(
                restored,
                currentActor,
                2))
            .thenCompose(committed -> {
                if (committed.cursor() == 3) {
                    return CompletableFuture.completedFuture(committed);
                }
                ZLinkActor actor = actorResolver.apply(committed.actorId());
                if (actor == null) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "target Actor is not materialized for Join completion"));
                }
                ActorRef publicRef = ZLinkActorRuntime.toPublicActorRef(
                    currentActor,
                    meshName);
                ZLinkMessage reply = committed.rawReply().length == 0
                    ? ZLinkMessage.empty()
                    : ZLinkMessage.fromEncoded(
                        ZLinkEncodedPayload.from(committed.rawReply()),
                        serializer);
                return mailbox.apply(
                        committed.actorId(),
                        () -> {
                            streamTrace("callback-start actor=" + committed.actorId()
                                + " operation=" + committed.operationId());
                            CompletionStage<Void> callback =
                                actor.onJoinCompleted(
                                    new ZLinkActorJoinCompletion.Accepted(
                                        committed.operationId(),
                                        publicRef,
                                        reply));
                            CompletionStage<Void> normalized = callback == null
                                ? CompletableFuture.completedFuture(null)
                                : callback;
                            normalized.whenComplete((ignored, error) ->
                                streamTrace("callback-complete actor="
                                    + committed.actorId()
                                    + " operation=" + committed.operationId()
                                    + " error=" + (error == null ? "none" : error)));
                            return normalized;
                        })
                    .thenCompose(ignored -> authorityJournal.advance(
                        committed,
                        currentActor,
                        3));
            })
            .thenApply(deliveredRoot -> null);
    }

    private static void streamTrace(String message) {
        if (STREAM_TRACE) {
            LOGGER.warning("[zlink-java-stream-trace] deferred-join " + message);
        }
    }

    CompletionStage<Void> completeSourceCleanup(
        Manifest manifest,
        ZLinkBackendActorRef currentActor) {
        if (authorityJournal == null || manifest.actorId().isEmpty()) {
            return CompletableFuture.completedFuture(null);
        }
        var operationId = new ZLinkActorJoinOperationId(
            manifest.operationIdHigh(),
            manifest.operationIdLow());
        return authorityJournal.recover(operationId, currentActor)
            .thenCompose(delivered ->
                authorityJournal.completeSourceCleanupAndRelease(
                    delivered,
                    currentActor));
    }

    CompletionStage<Void> markSourceCleanup(
        Manifest manifest,
        ZLinkBackendActorRef currentActor) {
        if (authorityJournal == null || manifest.actorId().isEmpty()) {
            return CompletableFuture.completedFuture(null);
        }
        return authorityJournal.markSourceCleanup(
            new ZLinkActorJoinOperationId(
                manifest.operationIdHigh(),
                manifest.operationIdLow()),
            currentActor);
    }

    private static byte[] encode(
        ZLinkActorJoinOperationId operationId,
        ZLinkBackendActorRef actor,
        byte[] rawReply) {
        try {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            try (DataOutputStream output = new DataOutputStream(bytes)) {
                output.writeInt(FORMAT_VERSION);
                output.writeLong(operationId.high());
                output.writeLong(operationId.low());
                output.writeUTF(actor.actorId());
                output.writeLong(actor.generation());
                byte[] reply = rawReply == null ? new byte[0] : rawReply.clone();
                output.writeInt(reply.length);
                output.write(reply);
            }
            return bytes.toByteArray();
        } catch (IOException impossible) {
            throw new IllegalStateException(impossible);
        }
    }

    private static Stored decode(byte[] payload) {
        try (DataInputStream input =
                 new DataInputStream(new ByteArrayInputStream(payload))) {
            if (input.readInt() != FORMAT_VERSION) {
                throw new IllegalStateException(
                    "unsupported deferred Actor Join completion format");
            }
            ZLinkActorJoinOperationId operationId =
                new ZLinkActorJoinOperationId(input.readLong(), input.readLong());
            String actorId = input.readUTF();
            long objectGeneration = input.readLong();
            int replyLength = input.readInt();
            if (replyLength < 0 || replyLength > payload.length) {
                throw new IllegalStateException(
                    "invalid deferred Actor Join completion reply length");
            }
            byte[] reply = input.readNBytes(replyLength);
            if (reply.length != replyLength || input.available() != 0) {
                throw new IllegalStateException(
                    "truncated deferred Actor Join completion root");
            }
            return new Stored(operationId, actorId, objectGeneration, reply);
        } catch (IOException error) {
            throw new IllegalStateException(
                "invalid deferred Actor Join completion root",
                error);
        }
    }

    private static long crc32c(byte[] payload) {
        java.util.zip.CRC32C checksum = new java.util.zip.CRC32C();
        checksum.update(payload, 0, payload.length);
        return checksum.getValue();
    }

    record Manifest(
        String reference,
        long checksumCrc32c,
        int cursor,
        long operationIdHigh,
        long operationIdLow,
        String actorId,
        long objectGeneration,
        long aggregateIdHigh,
        long aggregateIdLow,
        long aggregateGeneration) {
        Manifest(String reference, long checksumCrc32c, int cursor) {
            this(reference, checksumCrc32c, cursor, 0, 0, "", 0, 0, 0, 0);
        }
        Manifest(
            String reference,
            long checksumCrc32c,
            int cursor,
            long operationIdHigh,
            long operationIdLow,
            String actorId,
            long objectGeneration) {
            this(
                reference,
                checksumCrc32c,
                cursor,
                operationIdHigh,
                operationIdLow,
                actorId,
                objectGeneration,
                0,
                0,
                0);
        }
        Manifest {
            Objects.requireNonNull(reference, "reference");
            if (reference.isBlank() || cursor != CURSOR_COMMITTED) {
                throw new IllegalArgumentException(
                    "invalid deferred Actor Join completion manifest");
            }
            Objects.requireNonNull(actorId, "actorId");
            boolean hasIdentity = operationIdHigh != 0
                || operationIdLow != 0
                || !actorId.isEmpty()
                || objectGeneration != 0;
            if (hasIdentity
                && ((operationIdHigh == 0 && operationIdLow == 0)
                    || actorId.isBlank()
                    || objectGeneration == 0)) {
                throw new IllegalArgumentException(
                    "incomplete deferred Actor Join completion identity");
            }
            boolean hasAggregate = aggregateIdHigh != 0
                || aggregateIdLow != 0
                || aggregateGeneration != 0;
            if (hasAggregate
                && ((aggregateIdHigh == 0 && aggregateIdLow == 0)
                    || aggregateGeneration <= 0)) {
                throw new IllegalArgumentException(
                    "incomplete direct Join aggregate fence");
            }
        }

        boolean hasAggregateFence() {
            return aggregateGeneration > 0;
        }
    }

    record PreparedRoot(
        byte[] applicationState,
        List<ZLinkAsyncSerialQueue.QueuedRecord> acceptedJournal,
        byte[] sessionRouteCommand44) {
        PreparedRoot {
            applicationState = applicationState.clone();
            acceptedJournal = List.copyOf(acceptedJournal);
            sessionRouteCommand44 = sessionRouteCommand44.clone();
        }
        @Override public byte[] applicationState() {
            return applicationState.clone();
        }
        @Override public byte[] sessionRouteCommand44() {
            return sessionRouteCommand44.clone();
        }
    }

    private record Stored(
        ZLinkActorJoinOperationId operationId,
        String actorId,
        long objectGeneration,
        byte[] rawReply) {
    }
}
