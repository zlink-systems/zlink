package systems.zlink.framework.streams;

import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.messaging.ZLinkMessage;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public interface ZLinkSessionActor {
    String actorId();

    ActorRef ref();

    CompletionStage<Void> relay(ZLinkMessage payload);

    default CompletionStage<Void> relay(
            ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
        if (dispatch == null) {
            return CompletableFuture.failedFuture(
                    new IllegalArgumentException("dispatch is required"));
        }
        return relay(payload);
    }

    CompletionStage<Void> notifyDisconnected();
}
