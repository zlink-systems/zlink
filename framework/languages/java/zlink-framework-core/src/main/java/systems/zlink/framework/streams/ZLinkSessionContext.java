package systems.zlink.framework.streams;

import java.util.Optional;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;

public interface ZLinkSessionContext {
    String sessionId();

    Optional<RoutingId> routingId();

    Optional<String> localAddr();

    Optional<String> remoteAddr();

    ZLinkSessionClient client();

    ZLinkSessionActors actors();

    CompletionStage<Void> close();
}
