package systems.zlink.framework.actors;

import java.util.Optional;
import java.util.concurrent.CompletionStage;

public interface ZLinkActorManager {
    /**
     * Creates an actor with a globally unique actor id. The actor type string is
     * used only by creation APIs and must match a value registered with
     * {@code addActorFactory}.
     */
    ZLinkActorCreateCall create(String actorId, String actorType);

    /**
     * Looks up an existing actor by its globally unique actor id. Actor type is
     * not part of the lookup key.
     */
    CompletionStage<Optional<ActorRef>> find(String actorId);

    /**
     * Looks up the Spot the actor currently belongs to. Returns an empty result
     * when the actor is unknown or is not a member of any Spot.
     */
    CompletionStage<Optional<systems.zlink.framework.spots.SpotRef>> findSpot(String actorId);

    /**
     * Finds an actor by id or creates it using the factory registered for the
     * supplied actor type string.
     */
    ZLinkActorGetOrCreateCall getOrCreate(String actorId, String actorType);
}
