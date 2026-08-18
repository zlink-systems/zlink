package systems.zlink.framework.runtime.spots;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
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

    CompletionStage<Void> relay(
        RoutingId targetNodeRid,
        ZLinkSpotRetireControl.Fence fence,
        byte[] frozenRecord,
        Duration timeout);

    CompletionStage<Void> publish(
        RoutingId targetNodeRid,
        ZLinkSpotRetireControl.Fence fence,
        Duration timeout);

    CompletionStage<Void> abort(
        RoutingId targetNodeRid,
        ZLinkSpotRetireControl.Fence fence,
        Duration timeout);

    /**
     * True when this transition client can stream a pre-seal base snapshot
     * as command 52 chunks ahead of PREPARE (spec 15 §5). The legacy
     * request/reply control has no chunk stream and stays false; a base/
     * delta-capable adapter combined with a client that answers false must
     * take the unchanged full Capture/Restore path instead.
     */
    default boolean supportsBaseTransfer() {
        return false;
    }

    /**
     * Streams a captured base snapshot ahead of the eventual PREPARE for the
     * given exact relocation identity (spec 15 §5, spec 28 §4.2). Only
     * called when {@link #supportsBaseTransfer()} is true.
     * {@code advertisedReceiveChunkLimitBytes} additionally bounds this
     * relocation's chunk size (0 = not advertised, node budget only).
     */
    default CompletionStage<Void> sendBase(
        RoutingId targetNodeRid,
        ZLinkSpotRetireControl.Fence fence,
        ZLinkCanonicalRelocationProtocol.Coordinator coordinator,
        ZLinkCanonicalRelocationProtocol.ObjectFence object,
        byte[] base,
        long advertisedReceiveChunkLimitBytes,
        Duration timeout) {
        return CompletableFuture.failedFuture(new UnsupportedOperationException(
            "this relocation transition client cannot stream a base snapshot"));
    }
}
