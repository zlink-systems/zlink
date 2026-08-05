package systems.zlink.stream.connector;

import java.util.concurrent.CompletionStage;

@FunctionalInterface
public interface ZLinkStreamMessageHandler<TPayload> {
    CompletionStage<Void> handleAsync(ZLinkStreamMessage<TPayload> message);
}
