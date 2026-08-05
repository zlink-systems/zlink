package systems.zlink.stream.connector;

import java.util.concurrent.CompletionStage;

@FunctionalInterface
public interface ZLinkStreamConnectionStateHandler {
    CompletionStage<Void> handleAsync(ZLinkStreamConnectionState state);
}
