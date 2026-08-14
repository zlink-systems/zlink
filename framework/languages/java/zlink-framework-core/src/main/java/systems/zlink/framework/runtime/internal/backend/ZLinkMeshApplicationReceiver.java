package systems.zlink.framework.runtime.internal.backend;

import java.util.List;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;


public interface ZLinkMeshApplicationReceiver extends Consumer<ZLinkMeshDispatchRecord> {
    default void setLocalNodeReadyHandler(Runnable handler) {
    }

    CompletionStage<Integer> submitLocalNodeSend(
        RoutingId sourceNodeRid,
        byte[] metadata,
        List<Message> parts);
}
