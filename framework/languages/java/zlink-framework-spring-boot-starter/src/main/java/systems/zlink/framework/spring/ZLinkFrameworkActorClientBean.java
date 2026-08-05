package systems.zlink.framework.spring;

import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorRequestCall;
import systems.zlink.framework.actors.ZLinkActorSendCall;
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;

final class ZLinkFrameworkActorClientBean implements ZLinkActorClient {
    private final ZLinkFrameworkLifecycle lifecycle;

    ZLinkFrameworkActorClientBean(ZLinkFrameworkLifecycle lifecycle) {
        this.lifecycle = lifecycle;
    }

    @Override
    public ZLinkActorSendCall sendToActor(String actorId, Object message) {
        return lifecycle.actorClient().sendToActor(actorId, message);
    }

    @Override
    public ZLinkActorRequestCall requestToActor(String actorId, Object request) {
        return lifecycle.actorClient().requestToActor(actorId, request);
    }
}
