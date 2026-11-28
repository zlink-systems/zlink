package systems.zlink.stream.connector;

import java.util.concurrent.CompletionStage;

@FunctionalInterface
public interface ZLinkStreamAdmissionPositionHandler {
    CompletionStage<Void> handle(int position, int estimatedWaitMs);
}
