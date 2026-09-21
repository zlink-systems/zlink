package systems.zlink.framework.locations;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.*;

import java.util.concurrent.CompletionStage;

public interface ZLinkLocationReadiness {
    CompletionStage<Boolean> isPeerReady(
            String meshName, ZLinkLocationRole role, RoutingId nodeRid);
}
