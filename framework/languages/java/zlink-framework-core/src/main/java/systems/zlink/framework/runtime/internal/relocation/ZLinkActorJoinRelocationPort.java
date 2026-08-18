package systems.zlink.framework.runtime.internal.relocation;

import java.time.Duration;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.CompletionStage;
import java.util.function.Function;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.actors.ZLinkActorJoinOperationId;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;

/** Internal goal/profile seam from public Actor Join to canonical relocation. */
public interface ZLinkActorJoinRelocationPort {
    CompletionStage<Submission> relocate(Goal goal, Duration timeout);

    void admit(Admission admission);

    record Goal(
        UUID relocationId,
        ZLinkActorJoinOperationId operationId,
        ZLinkBackendActorRef sourceActor,
        String actorType,
        String targetSpotId,
        long targetSpotGeneration,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        long targetSpotAuthorityOwnerGeneration,
        long targetOwnerLeaseGeneration,
        byte[] rawReply,
        //  Non-null only for a deferred Join whose mailbox barrier is the
        //  actor queue's active turn: source preparation must seal that
        //  exact active turn instead of reserving a new lifecycle boundary
        //  that would queue behind the barrier itself and never activate.
        ZLinkAsyncSerialQueue.ActiveTurnSealHandle activeTurnSeal) {
        public Goal {
            Objects.requireNonNull(relocationId, "relocationId");
            Objects.requireNonNull(operationId, "operationId");
            Objects.requireNonNull(sourceActor, "sourceActor");
            Objects.requireNonNull(actorType, "actorType");
            Objects.requireNonNull(targetSpotId, "targetSpotId");
            Objects.requireNonNull(targetNodeRid, "targetNodeRid");
            rawReply = Objects.requireNonNull(rawReply, "rawReply").clone();
            if (relocationId.equals(new UUID(0L, 0L))
                || targetSpotGeneration <= 0
                || targetNodeGeneration <= 0
                || targetSpotAuthorityOwnerGeneration <= 0
                || targetOwnerLeaseGeneration <= 0) {
                throw new IllegalArgumentException(
                    "direct Join relocation target fence is invalid");
            }
        }

        @Override public byte[] rawReply() {
            return rawReply.clone();
        }
    }

    record Admission(
        UUID relocationId,
        ZLinkActorJoinOperationId operationId,
        String actorId,
        //  actorType and objectGeneration identify the object the
        //  relocation temporary queue prewarm is registered for (spec 15
        //  §4.2, spec 28 §3 exact identity == relocationId here since
        //  targetAttemptGeneration/coordinator fence are not known yet at
        //  admission time).
        String actorType,
        long objectGeneration,
        String targetSpotId,
        Object targetSpot,
        Function<ZLinkActor, CompletionStage<Void>> joined,
        ZLinkMessage reply,
        Duration timeout) {
        public Admission {
            Objects.requireNonNull(relocationId, "relocationId");
            Objects.requireNonNull(operationId, "operationId");
            Objects.requireNonNull(actorId, "actorId");
            Objects.requireNonNull(actorType, "actorType");
            Objects.requireNonNull(targetSpotId, "targetSpotId");
            Objects.requireNonNull(targetSpot, "targetSpot");
            Objects.requireNonNull(joined, "joined");
            Objects.requireNonNull(reply, "reply");
            if (objectGeneration <= 0) {
                throw new IllegalArgumentException(
                    "direct Join admission object generation must be positive");
            }
            if (timeout == null || timeout.isZero() || timeout.isNegative()) {
                throw new IllegalArgumentException(
                    "direct Join admission timeout must be positive");
            }
        }

    }

    record Submission(ZLinkBackendActorRef targetActor) {
        public Submission {
            Objects.requireNonNull(targetActor, "targetActor");
        }
    }
}
