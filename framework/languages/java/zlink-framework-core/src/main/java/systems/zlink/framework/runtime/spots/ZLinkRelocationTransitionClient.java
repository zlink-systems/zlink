package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;

import java.time.Duration;
import java.util.concurrent.CompletionStage;

/** Framework-private relocation transition boundary used by source schedulers. */
interface ZLinkRelocationTransitionClient {
    CompletionStage<Void> stage(
            RoutingId targetNodeRid, ZLinkSpotRetireControl.StageRequest request, Duration timeout);

    CompletionStage<Void> relay(
            RoutingId targetNodeRid,
            ZLinkSpotRetireControl.Fence fence,
            byte[] frozenRecord,
            Duration timeout);

    CompletionStage<Void> publish(
            RoutingId targetNodeRid, ZLinkSpotRetireControl.Fence fence, Duration timeout);

    CompletionStage<Void> abort(
            RoutingId targetNodeRid, ZLinkSpotRetireControl.Fence fence, Duration timeout);
}
