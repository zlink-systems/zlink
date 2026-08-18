package systems.zlink.framework.runtime.actors;

import java.util.Objects;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.BiFunction;
import java.util.function.Function;
import java.util.function.Supplier;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.actors.ZLinkActorJoinOperationId;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkDirectJoinRelocationAuthority;

/** Owns one live direct-Join relocation and its target Accepted callback. */
final class ZLinkDirectJoinRelocation {
    private final ZLinkMessageSerializer serializer;
    private final ZLinkDirectJoinRelocationAuthority relocationAuthority;

    ZLinkDirectJoinRelocation(
        ZLinkLocationRepository authority,
        ZLinkMessageSerializer serializer) {
        this.serializer = Objects.requireNonNull(serializer, "serializer");
        this.relocationAuthority = new ZLinkDirectJoinRelocationAuthority(
            Objects.requireNonNull(authority, "authority"));
    }

    CompletionStage<Manifest> prepareRelocation(
        UUID relocationId,
        ZLinkActorJoinOperationId operationId,
        ZLinkBackendActorRef actor,
        String actorType,
        String targetSpotId,
        RoutingId targetNodeRid,
        boolean restoreSnapshot,
        byte[] applicationState,
        List<ZLinkAsyncSerialQueue.QueuedRecord> acceptedJournal,
        byte[] rawReply) {
        return relocationAuthority.prepareRelocation(
                relocationId,
                actor,
                actorType,
                targetSpotId,
                targetNodeRid,
                restoreSnapshot,
                applicationState,
                acceptedJournal)
            .thenApply(value -> new Manifest(
                value.root(),
                value.checksumCrc32c(),
                operationId.high(),
                operationId.low(),
                value.fence().aggregateId().getMostSignificantBits(),
                value.fence().aggregateId().getLeastSignificantBits(),
                value.fence().aggregateGeneration(),
                rawReply));
    }

    CompletionStage<ZLinkDirectJoinRelocationAuthority.CommittedActorTenure>
        commitPrepared(
        Manifest manifest,
        ZLinkBackendActorRef actor) {
        if (!manifest.hasAggregateFence()) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "direct Actor Join relocation fence is missing"));
        }
        return relocationAuthority.commitPrepared(
            new UUID(
                manifest.aggregateIdHigh(),
                manifest.aggregateIdLow()),
            manifest.aggregateGeneration(),
            actor);
    }

    CompletionStage<Void> publishTargetReady(
        Manifest manifest,
        ZLinkBackendActorRef actor) {
        if (!manifest.hasAggregateFence()) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "direct Actor Join relocation fence is missing"));
        }
        return relocationAuthority.publishTargetReady(actor);
    }

    CompletionStage<PreparedRoot> loadPrepared(
        Manifest manifest,
        ZLinkBackendActorRef actor,
        boolean restoreSnapshot) {
        if (!manifest.hasAggregateFence()) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "direct Actor Join relocation fence is missing"));
        }
        return relocationAuthority.loadPrepared(
                manifest.root(),
                manifest.checksumCrc32c(),
                actor,
                new UUID(
                    manifest.aggregateIdHigh(),
                    manifest.aggregateIdLow()),
                restoreSnapshot)
            .thenApply(root -> new PreparedRoot(
                root.applicationState(),
                root.acceptedJournal()));
    }

    CompletionStage<Void> abortPrepared(Manifest manifest) {
        if (!manifest.hasAggregateFence()) {
            return CompletableFuture.completedFuture(null);
        }
        return relocationAuthority.abortPrepared(
            new UUID(
                manifest.aggregateIdHigh(),
                manifest.aggregateIdLow()),
            manifest.aggregateGeneration());
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
        ZLinkActorJoinOperationId operationId =
            new ZLinkActorJoinOperationId(
                manifest.operationIdHigh(),
                manifest.operationIdLow());
        ZLinkActor actor = actorResolver.apply(currentActor.actorId());
        if (actor == null) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "target Actor is not materialized for Join completion"));
        }
        ActorRef publicRef = ZLinkActorRuntime.toPublicActorRef(
            currentActor,
            meshName);
        ZLinkMessage reply = manifest.rawReply().length == 0
            ? ZLinkMessage.empty()
            : ZLinkMessage.fromEncoded(
                ZLinkEncodedPayload.from(manifest.rawReply()),
                serializer);
        return mailbox.apply(
                currentActor.actorId(),
                () -> actor.onJoinCompleted(
                    new ZLinkActorJoinCompletion.Accepted(
                        operationId,
                        publicRef,
                        reply)));
    }

    record Manifest(
        byte[] root,
        long checksumCrc32c,
        long operationIdHigh,
        long operationIdLow,
        long aggregateIdHigh,
        long aggregateIdLow,
        long aggregateGeneration,
        byte[] rawReply) {
        Manifest {
            root = Objects.requireNonNull(root, "root").clone();
            if (root.length == 0) {
                throw new IllegalArgumentException(
                    "invalid direct Actor Join relocation manifest");
            }
            rawReply = Objects.requireNonNull(rawReply, "rawReply").clone();
            if (operationIdHigh == 0 && operationIdLow == 0) {
                throw new IllegalArgumentException(
                    "direct Actor Join operation identity is missing");
            }
            if ((aggregateIdHigh == 0 && aggregateIdLow == 0)
                || aggregateGeneration <= 0) {
                throw new IllegalArgumentException(
                    "direct Join aggregate fence is missing");
            }
        }

        boolean hasAggregateFence() {
            return aggregateGeneration > 0;
        }

        @Override public byte[] root() {
            return root.clone();
        }

        @Override public byte[] rawReply() {
            return rawReply.clone();
        }
    }

    record PreparedRoot(
        byte[] applicationState,
        List<ZLinkAsyncSerialQueue.QueuedRecord> acceptedJournal) {
        PreparedRoot {
            applicationState = applicationState.clone();
            acceptedJournal = List.copyOf(acceptedJournal);
        }
        @Override public byte[] applicationState() {
            return applicationState.clone();
        }
    }

}
