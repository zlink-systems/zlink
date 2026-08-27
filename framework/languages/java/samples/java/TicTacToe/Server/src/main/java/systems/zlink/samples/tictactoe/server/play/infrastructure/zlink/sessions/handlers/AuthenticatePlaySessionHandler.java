package systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.sessions.handlers;
import java.util.concurrent.CompletionStage;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;
import systems.zlink.samples.tictactoe.server.configuration.SampleNames;
import systems.zlink.samples.tictactoe.shared.contracts.AuthenticatePlayerReq;
import systems.zlink.samples.tictactoe.shared.contracts.AuthenticatePlayerRes;
import systems.zlink.samples.tictactoe.shared.contracts.AuthenticateReq;
import systems.zlink.samples.tictactoe.shared.contracts.AuthenticateRes;
import systems.zlink.samples.tictactoe.shared.contracts.PlayerActorCreateReq;

// --8<-- [start:doc-session-auth]
public final class AuthenticatePlaySessionHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, AuthenticateReq> {
    private static final Logger LOGGER = LoggerFactory.getLogger(
        AuthenticatePlaySessionHandler.class);

    private final ZLinkActorManager actors;
    private final ZLinkClient channels;

    public AuthenticatePlaySessionHandler(
        ZLinkActorManager actors,
        ZLinkClient channels) {
        this.actors = actors;
        this.channels = channels;
    }

    @Override
    public Class<AuthenticateReq> messageType() {
        return AuthenticateReq.class;
    }

    @Override
    public CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        AuthenticateReq request) {
        if (request.accessToken() == null || request.accessToken().isBlank()) {
            throw new IllegalArgumentException("access token is required");
        }
        return channels
            .requestToChannel(
                SampleNames.ApiChannel,
                new AuthenticatePlayerReq(request.accessToken()))
            .timeout(SampleNames.RequestTimeout)
            .submit(AuthenticatePlayerRes.class)
            .thenCompose(authenticated -> actors.getOrCreate(
                    authenticated.player().actorId(), SampleNames.PlayActor)
                .request(new PlayerActorCreateReq(authenticated.player()))
                .submit()
                .thenCompose(result -> {
                    ActorRef resolvedActor = requireActor(result);
                    return context.actors().bind(requireActor(result))
                        .thenCompose(boundActor -> {
                            if (!boundActor.ref().equals(resolvedActor)) {
                                throw new IllegalStateException(
                                    "Bound ActorRef does not match the resolved ActorRef for '"
                                        + authenticated.player().actorId() + "'.");
                            }
                            if (result instanceof ZLinkActorCreateResult.Existing) {
                                LOGGER.info(
                                    "tictactoe-lifecycle actor-bound actor={}",
                                    boundActor.actorId());
                            }
                            return context.client()
                                .reply(new AuthenticateRes(authenticated.player()))
                                .submit();
                        });
                }));
    }

    private static ActorRef requireActor(ZLinkActorCreateResult result) {
        if (result instanceof ZLinkActorCreateResult.Existing existing) {
            return existing.actor();
        }
        if (result instanceof ZLinkActorCreateResult.Created created) {
            return created.actor();
        }
        throw new IllegalStateException("Play actor creation was rejected.");
    }
}
// --8<-- [end:doc-session-auth]
