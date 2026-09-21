package systems.zlink.framework.spring;

import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorDirectory;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;

import java.util.Optional;
import java.util.concurrent.CompletionStage;

final class ZLinkFrameworkActorDirectoryBean implements ZLinkActorDirectory {
    private final ZLinkFrameworkLifecycle lifecycle;

    ZLinkFrameworkActorDirectoryBean(ZLinkFrameworkLifecycle lifecycle) {
        this.lifecycle = lifecycle;
    }

    @Override
    public CompletionStage<Optional<ActorRef>> find(String actorId) {
        return lifecycle.actorDirectory().find(actorId);
    }

    @Override
    public CompletionStage<ActorRef> ensure(String actorId, ZLinkMessage createRequest) {
        return lifecycle.actorDirectory().ensure(actorId, createRequest);
    }
}
