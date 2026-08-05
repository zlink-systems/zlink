package systems.zlink.framework.runtime.internal.backend;

import java.util.List;
import java.util.function.Consumer;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;


public interface ZLinkMeshApplicationReceiver extends Consumer<ZLinkMeshDispatchRecord> {
    default void setLocalNodeReadyHandler(Runnable handler) {
    }

    /** Returns whether the next application receive may enter Framework dispatch. */
    default boolean canReceiveApplication() {
        return true;
    }

    /** Installs the wake-up callback used when application receive capacity returns. */
    default void setApplicationReceiveReadyHandler(Runnable handler) {
    }

    /** Returns the host-wide budget used by this application receiver. */
    default ZLinkInboundDispatchBudget applicationDispatchBudget() {
        return null;
    }

    int submitLocalNodeSend(
        RoutingId sourceNodeRid,
        byte[] metadata,
        List<Message> parts);
}
