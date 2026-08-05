package systems.zlink.framework.spots;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.messaging.ZLinkMessage;

/**
 * Admission and membership lifecycle used only by User Spots.
 */
public interface ZLinkUserSpotActorLifecycle<TActor extends ZLinkActor>
    extends ZLinkSpotActorMembershipLifecycle<TActor> {
    default CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
        String actorId,
        ZLinkMessage request) {
        return java.util.concurrent.CompletableFuture.completedFuture(
            ZLinkSpotActorJoinResult.reject());
    }
}
