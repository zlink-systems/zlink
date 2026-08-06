package systems.zlink.e2e.automaticturn.shared;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;

public final class AwaitActor implements ZLinkActor {
    private final ZLinkActorContext context;

    public AwaitActor(ZLinkActorContext context) {
        this.context = context;
    }

    public String actorId() {
        return context.actorId();
    }

    @Override
    public ZLinkActorContext context() {
        return context;
    }
}
