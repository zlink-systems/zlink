package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;

import java.time.Duration;
import java.time.Instant;
import java.util.concurrent.CompletionStage;

/** Framework-private relocation transition boundary used by source schedulers. */
interface ZLinkRelocationTransitionClient {
    /** The source transport's terminal lifecycle, independent of remote failure replies. */
    default boolean sourceStopped() {
        return false;
    }

    /**
     * Stages one attempt. {@code deadline} is the absolute deadline of the operation that started
     * the relocation; command 40 carries it so the target bounds its relay-ready preparation
     * (relocation §4.3). Every PREPARE of the attempt carries the same value.
     */
    CompletionStage<Void> stage(
            RoutingId targetNodeRid,
            ZLinkSpotRetireControl.StageRequest request,
            Duration timeout,
            Instant deadline);

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

    /** Carries the current first-target Follow fence; absent after a subsequent owner commits. */
    default CompletionStage<AuthoritySettlement> settleAuthority(
            RoutingId targetNodeRid, ZLinkSpotRetireControl.Fence fence, Instant preserveAt) {
        return settle(targetNodeRid, fence, preserveAt)
                .thenApply(outcome -> new AuthoritySettlement(outcome, null));
    }

    record AuthoritySettlement(
            Settlement outcome,
            systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot target) {}

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
