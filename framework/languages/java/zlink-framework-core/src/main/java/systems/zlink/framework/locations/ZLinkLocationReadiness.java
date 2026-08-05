package systems.zlink.framework.locations;

import systems.zlink.framework.runtime.internal.locations.*;

import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;

public interface ZLinkLocationReadiness {
    CompletionStage<Boolean> isPeerReady(
        String meshName,
        ZLinkLocationRole role,
        RoutingId nodeRid);
}
