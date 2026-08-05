package systems.zlink.framework.streams;

import systems.zlink.framework.messaging.ZLinkMessage;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public interface ZLinkSession {
    ZLinkSessionContext context();

    CompletionStage<Void> onConnected();

    CompletionStage<Void> onDisconnected();

    CompletionStage<Void> onError(ZLinkStreamError error);

    default CompletionStage<Void> onDispatch(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        return CompletableFuture.completedFuture(null);
    }
}
