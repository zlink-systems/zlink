package systems.zlink.framework.actors;
import systems.zlink.framework.spots.SpotRef;

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
    CompletionStage<Optional<SpotRef>> findSpot(String actorId);

    /**
     * Destroys the exact actor incarnation identified by {@code actor}.
     *
     * <p>A missing incarnation is idempotent and completes with {@code false}.
     * A generation mismatch is rejected with {@code INVALID_OPERATION}; the
     * manager never retargets the request to a newer actor with the same id.
     * The actor must already have left its user Spot.</p>
     */
    CompletionStage<Boolean> destroy(ActorRef actor);

    /**
     * Finds an actor by id or creates it using the factory registered for the
     * supplied actor type string.
     */
    ZLinkActorGetOrCreateCall getOrCreate(String actorId, String actorType);
}
