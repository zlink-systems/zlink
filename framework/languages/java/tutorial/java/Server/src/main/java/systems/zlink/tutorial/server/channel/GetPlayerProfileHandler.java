package systems.zlink.tutorial.server.channel;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.tutorial.shared.Contracts;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

// --8<-- [start:channel-request-handler]
// Answers a request addressed to the "profile" channel. Any node that exposes
// this channel may receive it; the caller does not pick one.
public final class GetPlayerProfileHandler
        implements ZLinkRequestHandler<Contracts.GetPlayerProfile, Contracts.PlayerProfile> {

    @Override
    public CompletionStage<Contracts.PlayerProfile> handle(
            Contracts.GetPlayerProfile request, ZLinkMessageContext context) {
        var profile = new Contracts.PlayerProfile(request.playerId(), "rookie", 1);
        return CompletableFuture.completedFuture(profile);
    }
}
// --8<-- [end:channel-request-handler]
