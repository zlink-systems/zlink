package systems.zlink.framework.perf;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;

// Actor echo object shared by the CS (§10.1, §10.2) and Actor (§10.9, §10.10) Object Servers.
// The Actor holds no state: every measured call is the typed echo of the Entry Spot's Actor handler.
public final class PerfActor implements ZLinkActor {
    private final ZLinkActorContext context;

    public PerfActor(ZLinkActorContext context) {
        this.context = context;
    }

    @Override
    public ZLinkActorContext context() {
        return context;
    }
}
