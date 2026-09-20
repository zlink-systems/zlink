package systems.zlink.samples.deliverydispatch.server.customergateway;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;

import java.util.concurrent.CompletionStage;

public final class CustomerActor implements ZLinkActor {
    private final String actorId;
    private final ZLinkActorContext context;

    public CustomerActor(String actorId, ZLinkActorContext context) {
        this.actorId = actorId;
        this.context = context;
    }

    public String actorId() {
        return actorId;
    }

    @Override
    public ZLinkActorContext context() {
        return context;
    }

    public CompletionStage<Void> push(Object message) {
        return context.boundSession().send(message).submit();
    }
}
