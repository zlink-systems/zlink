package systems.zlink.framework.spots;

import java.util.Optional;
import java.util.concurrent.CompletionStage;

public interface SpotHandleResolver {
    CompletionStage<Optional<SpotHandle>> resolveSpotHandle(String meshName, String spotId);

    CompletionStage<Optional<SpotHandle>> resolveSpotHandle(String spotId);
}
