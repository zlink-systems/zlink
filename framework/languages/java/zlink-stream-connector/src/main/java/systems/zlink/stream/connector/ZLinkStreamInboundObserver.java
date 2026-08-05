package systems.zlink.stream.connector;

import java.util.concurrent.CompletionStage;

@FunctionalInterface
public interface ZLinkStreamInboundObserver {
    CompletionStage<Void> observeAsync(ZLinkStreamInboundObservation observation);
}
