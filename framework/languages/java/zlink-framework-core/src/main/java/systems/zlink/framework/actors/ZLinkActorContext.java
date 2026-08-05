package systems.zlink.framework.actors;

import java.util.Optional;

public interface ZLinkActorContext {
    String actorId();

    long objectGeneration();

    String meshName();

    Optional<String> spotId();

    ZLinkBoundSession boundSession();

    ZLinkActorJoinCall joinSpot(String spotId);

    ZLinkActorJoinCall joinSpot(String spotId, Object request);

    ZLinkActorJoinCall joinEntrySpot();

    ZLinkActorJoinCall joinEntrySpot(Object request);
}
