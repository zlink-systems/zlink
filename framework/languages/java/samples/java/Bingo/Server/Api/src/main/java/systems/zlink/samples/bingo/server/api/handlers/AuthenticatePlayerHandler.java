package systems.zlink.samples.bingo.server.api.handlers;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.samples.bingo.server.configuration.SampleNames;
import systems.zlink.samples.bingo.shared.contracts.BingoMessages;
import systems.zlink.samples.bingo.shared.contracts.Messages;

@ZLinkHandlerGroup(SampleNames.ApiChannel)
public final class AuthenticatePlayerHandler
    implements ZLinkRequestHandler<
        Messages.AuthenticatePlayerReq,
        Messages.AuthenticatePlayerRes> {
    @Override
    public CompletionStage<Messages.AuthenticatePlayerRes> handle(
        Messages.AuthenticatePlayerReq request,
        ZLinkMessageContext context) {
        String actorId = request.getAccessToken();
        return CompletableFuture.completedFuture(
            BingoMessages.authenticatePlayerRes(true, actorId, actorId, null));
    }
}
