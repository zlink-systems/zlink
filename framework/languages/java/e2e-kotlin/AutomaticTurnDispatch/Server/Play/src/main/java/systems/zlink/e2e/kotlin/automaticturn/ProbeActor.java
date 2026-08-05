package systems.zlink.e2e.kotlin.automaticturn;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;

public final class ProbeActor implements ZLinkActor {
    private final String actorId;
    private final ZLinkActorContext context;

    public ProbeActor(String actorId, ZLinkActorContext context) {
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
