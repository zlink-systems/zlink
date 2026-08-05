package systems.zlink.samples.deliverydispatch.server.customergateway;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class CustomerActorFactory implements ZLinkActorFactory {
    @Override
    public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
        return CompletableFuture.completedFuture(new CustomerActor(context.actorId(), context));
    }
}
