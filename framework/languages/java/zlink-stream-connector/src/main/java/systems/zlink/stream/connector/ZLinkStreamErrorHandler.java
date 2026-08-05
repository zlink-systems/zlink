package systems.zlink.stream.connector;

import java.util.concurrent.CompletionStage;

@FunctionalInterface
public interface ZLinkStreamErrorHandler {
    CompletionStage<Void> handleAsync(ZLinkStreamError error);
}
