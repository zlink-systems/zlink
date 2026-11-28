package systems.zlink.stream.connector;

import java.util.concurrent.CompletionStage;

@FunctionalInterface
public interface ZLinkStreamAdmissionQueuedHandler {
    CompletionStage<Void> handle();
}
