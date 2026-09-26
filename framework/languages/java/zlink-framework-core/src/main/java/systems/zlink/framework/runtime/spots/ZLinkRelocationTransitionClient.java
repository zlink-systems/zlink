package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;

import java.time.Duration;
import java.time.Instant;
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

    /**
     * Waits for the definitive authority of a relocation attempt after relay readiness (spec 28
     * §4.4). A cutover submit terminal is not target success; only this settlement is. From {@code
     * preserveAt}, the unit's Restore absolute deadline, the source settles with its {@code
     * Preserve} fence (spec 01 §10).
     */
    CompletionStage<Settlement> settle(
            RoutingId targetNodeRid, ZLinkSpotRetireControl.Fence fence, Instant preserveAt);

    CompletionStage<Void> abort(
            RoutingId targetNodeRid, ZLinkSpotRetireControl.Fence fence, Duration timeout);

    /** Definitive authority outcome of one relocation unit (spec 28 §4.4, spec 01 §10). */
    enum Settlement {
        /** The Location Store shows the target owner: adopt the target route. */
        TARGET_COMMITTED,
        /** The source {@code Preserve} fence won: restore the retained work in order. */
        SOURCE_PRESERVED,
        /** The source owner lease expired before a definitive fence: expired-owner terminal. */
        SOURCE_LEASE_EXPIRED
    }
}
