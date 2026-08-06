package systems.zlink.e2e.runtimemonitoring.service.handlers;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;

public final class MonitoringActor implements ZLinkActor {
    public static final String TYPE = "monitoring-placement-actor";
    private final ZLinkActorContext context;

    public MonitoringActor(ZLinkActorContext context) {
        this.context = context;
    }

    @Override
    public ZLinkActorContext context() {
        return context;
    }
}
