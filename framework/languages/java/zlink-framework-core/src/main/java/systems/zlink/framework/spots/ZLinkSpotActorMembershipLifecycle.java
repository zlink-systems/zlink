package systems.zlink.framework.spots;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActor;

/**
 * Membership notifications shared by User Spots and Entry Spots.
 */
public interface ZLinkSpotActorMembershipLifecycle<TActor extends ZLinkActor> {
    CompletionStage<Void> onJoinedActor(TActor actor);

    CompletionStage<Void> onLeaveActor(TActor actor);

    default CompletionStage<Void> onDisconnectActor(TActor actor) {
        return CompletableFuture.completedFuture(null);
    }
}
