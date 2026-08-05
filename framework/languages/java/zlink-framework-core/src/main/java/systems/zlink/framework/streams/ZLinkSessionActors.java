package systems.zlink.framework.streams;

import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActor;

public interface ZLinkSessionActors {
    List<ZLinkSessionActor> bound();

    CompletionStage<ZLinkSessionActor> bind(ZLinkActor actor);

    CompletionStage<ZLinkSessionActor> bind(ActorRef actor);

    CompletionStage<ZLinkSessionActor> bindOrGet(ActorRef actor);

    Optional<ZLinkSessionActor> find(String actorId);
}
