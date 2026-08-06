package systems.zlink.e2e.kotlin.automaticturn;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;

public final class ProbeActor implements ZLinkActor {
    private final ZLinkActorContext context;

    public ProbeActor(ZLinkActorContext context) {
        this.context = context;
    }

    @Override
    public ZLinkActorContext context() {
        return context;
    }
}
