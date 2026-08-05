package systems.zlink.framework.runtime.spots;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;

/**
 * Framework-private relocation transition boundary used by source schedulers.
 */
interface ZLinkRelocationTransitionClient {
    CompletionStage<Void> stage(
        RoutingId targetNodeRid,
        ZLinkSpotRetireControl.StageRequest request,
        Duration timeout);

    CompletionStage<Void> publish(
        RoutingId targetNodeRid,
        ZLinkSpotRetireControl.Fence fence,
        Duration timeout);

    CompletionStage<Void> abort(
        RoutingId targetNodeRid,
        ZLinkSpotRetireControl.Fence fence,
        Duration timeout);

    CompletionStage<Void> finalizeAfterCompletion(
        RoutingId targetNodeRid,
        ZLinkSpotRetireControl.Fence fence,
        Duration timeout);
}
