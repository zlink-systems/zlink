package systems.zlink.samples.bingo.server.session.sessions.handlers;


import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;
import systems.zlink.samples.bingo.server.configuration.SampleNames;
import systems.zlink.samples.bingo.server.configuration.SampleTimings;
import systems.zlink.samples.bingo.shared.contracts.BingoMessages;
import systems.zlink.samples.bingo.shared.contracts.Messages;

public final class AuthenticateSessionHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Messages.AuthenticateReq> {
    private final ZLinkRouteClient routes;
    private final ZLinkActorManager actors;

    public AuthenticateSessionHandler(
        ZLinkRouteClient routes,
        ZLinkActorManager actors) {
        this.routes = routes;
        this.actors = actors;
    }

    @Override
    public Class<Messages.AuthenticateReq> messageType() {
        return Messages.AuthenticateReq.class;
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        Messages.AuthenticateReq request) {
        if (request.getAccessToken().isBlank()) {
            throw new IllegalArgumentException("access token is required");
        }
        return routes
            .requestToChannel(
                SampleNames.ApiChannel,
                BingoMessages.authenticatePlayerReq(request.getAccessToken()))
            .timeout(SampleTimings.RequestTimeout)
            .submit(Messages.AuthenticatePlayerRes.class)
            .thenCompose(authenticated -> {
                requireAuthenticated(authenticated);
                return actors.getOrCreate(
                        authenticated.getActorId(),
                        SampleNames.PlayerActorType)
                    .request(BingoMessages.ensurePlayerActorReq(
                            authenticated.getActorId(),
                            authenticated.getDisplayName()))
                    .submit()
                    .thenCompose(result -> context.actors().bind(requireActor(result))
                        .thenRun(() -> context.client().reply(BingoMessages.authenticateRes(
                            authenticated.getActorId(),
                            authenticated.getDisplayName())).submit()));
            });
    }

    private static void requireAuthenticated(Messages.AuthenticatePlayerRes authenticated) {
        if (!authenticated.getAccepted()
            || authenticated.getActorId().isBlank()
            || authenticated.getDisplayName().isBlank()) {
            throw new IllegalStateException(authenticated.getReason().isBlank()
                ? "Player authentication failed."
                : authenticated.getReason());
        }
    }

    private static ActorRef requireActor(ZLinkActorCreateResult result) {
        if (result instanceof ZLinkActorCreateResult.Existing existing) {
            return existing.actor();
        }
        if (result instanceof ZLinkActorCreateResult.Created created) {
            return created.actor();
        }
        throw new IllegalStateException("Player actor creation was rejected.");
    }
}
