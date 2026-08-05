package systems.zlink.framework.runtime.actors;

import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorDirectory;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;

public final class ZLinkStoreActorDirectory implements ZLinkActorDirectory {
    private final ZLinkStoreLocationResolvers locations;

    public ZLinkStoreActorDirectory(ZLinkStoreLocationResolvers locations) {
        this.locations = java.util.Objects.requireNonNull(locations, "locations");
    }

    @Override
    public CompletionStage<Optional<ActorRef>> find(String actorId) {
        if (actorId == null || actorId.isBlank()) {
            throw new ZLinkConfigurationException("actorId is required");
        }
        return locations.resolveActor(actorId)
            .thenApply(row -> row == null
                ? Optional.empty()
                : Optional.of(row.actorRef()));
    }

    @Override
    public CompletionStage<ActorRef> ensure(
        String actorId,
        ZLinkMessage createRequest) {
        return CompletableFuture.failedFuture(new ZLinkConfigurationException(
            "actor directory ensure requires an actor factory"));
    }
}
