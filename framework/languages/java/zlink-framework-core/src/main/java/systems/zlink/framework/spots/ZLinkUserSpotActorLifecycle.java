package systems.zlink.framework.spots;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.messaging.ZLinkMessage;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

/** Admission and membership lifecycle used only by User Spots. */
public interface ZLinkUserSpotActorLifecycle<TActor extends ZLinkActor>
        extends ZLinkSpotActorMembershipLifecycle<TActor> {
    default CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
            String actorId, ZLinkMessage request) {
        return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.reject());
    }
}
