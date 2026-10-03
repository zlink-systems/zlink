package systems.zlink.framework.runtime.internal.backend;

/**
 * A peer connection replacement found an earlier intent for the same endpoint whose close has
 * been requested but has not completed. The replacement attempt is not made; the intent owner
 * submits it again on its next reconcile (spec 03-mesh-node §connection intent).
 */
public final class ZLinkPeerIntentClosePendingException extends IllegalStateException {
    public ZLinkPeerIntentClosePendingException() {
        super("previous peer connection has not completed liveness close");
    }
}
