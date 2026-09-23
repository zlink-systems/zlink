package systems.zlink.stream.connector;

import java.util.concurrent.CompletionStage;

@FunctionalInterface
public interface ZLinkStreamActorHandler {
    CompletionStage<Void> handle(ZLinkStreamActor actor);
}
