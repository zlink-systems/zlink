package systems.zlink.stream.connector;

import java.util.concurrent.CompletionStage;

@FunctionalInterface
public interface ZLinkStreamAdmissionWaitingHandler {
    CompletionStage<Void> handle(ZLinkStreamAdmissionWaiting event);
}
