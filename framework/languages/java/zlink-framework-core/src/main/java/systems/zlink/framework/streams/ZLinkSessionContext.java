package systems.zlink.framework.streams;

import systems.zlink.contracts.core.RoutingId;

import java.util.Optional;
import java.util.concurrent.CompletionStage;

public interface ZLinkSessionContext {
    String sessionId();

    Optional<RoutingId> routingId();

    Optional<String> localAddr();

    Optional<String> remoteAddr();

    ZLinkSessionClient client();

    ZLinkSessionActors actors();

    CompletionStage<Void> close();
}
