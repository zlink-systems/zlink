package systems.zlink.framework.spring;

import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;

import java.util.Optional;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorCreateCall;
import systems.zlink.framework.actors.ZLinkActorGetOrCreateCall;
import systems.zlink.framework.actors.ZLinkActorManager;

final class ZLinkFrameworkActorManagerBean implements ZLinkActorManager {
    private final ZLinkFrameworkLifecycle lifecycle;

    ZLinkFrameworkActorManagerBean(ZLinkFrameworkLifecycle lifecycle) {
        this.lifecycle = lifecycle;
    }

    @Override
    public ZLinkActorCreateCall create(String actorId, String actorType) {
        return lifecycle.actorManager().create(actorId, actorType);
    }

    @Override
    public CompletionStage<Optional<ActorRef>> find(String actorId) {
        return lifecycle.actorManager().find(actorId);
    }

    @Override
    public CompletionStage<Optional<systems.zlink.framework.spots.SpotRef>> findSpot(
        String actorId) {
        return lifecycle.actorManager().findSpot(actorId);
    }

    @Override
    public ZLinkActorGetOrCreateCall getOrCreate(
        String actorId,
        String actorType) {
        return lifecycle.actorManager().getOrCreate(actorId, actorType);
    }
}
