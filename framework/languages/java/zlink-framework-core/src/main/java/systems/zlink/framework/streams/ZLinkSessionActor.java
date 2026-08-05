package systems.zlink.framework.streams;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.messaging.ZLinkMessage;

public interface ZLinkSessionActor {
    String actorId();

    ActorRef ref();

    CompletionStage<Void> relay(ZLinkMessage payload);

    default CompletionStage<Void> relay(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        if (dispatch == null) {
            return java.util.concurrent.CompletableFuture.failedFuture(
                new IllegalArgumentException("dispatch is required"));
        }
        return relay(payload);
    }

    CompletionStage<Void> notifyDisconnected();
}
