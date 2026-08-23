package systems.zlink.crosslanguage.host;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;

public final class RelocationActorFactory implements ZLinkActorFactory {
    private final EventSink sink;

    public RelocationActorFactory(EventSink sink) {
        this.sink = sink;
    }

    @Override
    public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
        return CompletableFuture.completedFuture(
            new RelocationActor(context.actorId(), context, sink));
    }
}
