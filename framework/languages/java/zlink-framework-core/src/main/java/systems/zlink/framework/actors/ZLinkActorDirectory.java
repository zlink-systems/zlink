package systems.zlink.framework.actors;

import java.util.Optional;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.messaging.ZLinkMessage;

public interface ZLinkActorDirectory {
    CompletionStage<Optional<ActorRef>> find(String actorId);

    CompletionStage<ActorRef> ensure(
        String actorId,
        ZLinkMessage createRequest);

    default CompletionStage<ActorRef> ensure(
        String actorId,
        Object createRequest) {
        return ensure(actorId, ZLinkMessage.of(createRequest));
    }
}
