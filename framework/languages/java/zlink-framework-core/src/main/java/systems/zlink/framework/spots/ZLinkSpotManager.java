package systems.zlink.framework.spots;

import java.util.Optional;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;

public interface ZLinkSpotManager {
    ZLinkSpotCreateCall create(String spotType);
    ZLinkSpotGetOrCreateCall getOrCreate(String spotId, String spotType);
    CompletionStage<Optional<SpotRef>> find(String spotId);
    CompletionStage<Boolean> close(SpotRef spot);
}
