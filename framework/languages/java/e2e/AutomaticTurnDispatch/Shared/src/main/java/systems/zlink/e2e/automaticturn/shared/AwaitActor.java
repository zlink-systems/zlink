package systems.zlink.e2e.automaticturn.shared;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;

public final class AwaitActor implements ZLinkActor {
    private final String actorId;
    private final ZLinkActorContext context;

    public AwaitActor(String actorId, ZLinkActorContext context) {
        this.actorId = actorId;
        this.context = context;
    }

    @Override
    public String actorId() {
        return actorId;
    }

    @Override
    public ZLinkActorContext context() {
        return context;
    }
}
