package systems.zlink.framework.spots;

import java.util.Optional;
import java.util.concurrent.CompletionStage;

public interface ActorSpotHandleResolver {
    CompletionStage<Optional<SpotHandle>> resolveActorSpotHandle(String actorId);
}
