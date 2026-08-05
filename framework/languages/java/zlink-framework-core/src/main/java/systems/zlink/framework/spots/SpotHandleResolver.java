package systems.zlink.framework.spots;

import java.util.Optional;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;

public interface SpotHandleResolver {
    CompletionStage<Optional<SpotHandle>> resolveSpotHandle(
        String meshName,
        String spotId);

    CompletionStage<Optional<SpotHandle>> resolveSpotHandle(String spotId);
}
