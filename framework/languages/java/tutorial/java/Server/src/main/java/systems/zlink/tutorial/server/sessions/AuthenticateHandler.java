package systems.zlink.tutorial.server.sessions;

import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;
import systems.zlink.tutorial.shared.Contracts;

import java.time.Duration;
import java.util.concurrent.CompletionStage;

// --8<-- [start:session-actor-bind]
// Ties this connection to one player. After this, packets without a session
// handler reach that player, and the player can push to this connection.
public final class AuthenticateHandler
        implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.Authenticate> {

    private final ZLinkActorManager players;

    public AuthenticateHandler(ZLinkActorManager players) {
        this.players = players;
    }

    @Override
    public Class<Contracts.Authenticate> messageType() {
        return Contracts.Authenticate.class;
    }

    @Override
    public CompletionStage<Void> handle(
            ZLinkSessionContext context,
            ZLinkSessionDispatchContext dispatch,
            Contracts.Authenticate message) {
        // A returning client finds its existing player rather than a new one.
        return players.getOrCreate(message.playerId(), "player")
                .inMesh("game")
                .request(new Contracts.CreatePlayer(message.playerId()))
                .timeout(Duration.ofSeconds(10))
                .submit()
                .thenCompose(result -> context.actors().bindOrGet(resolve(result)))
                .thenCompose(
                        bound ->
                                context.client()
                                        .reply(new Contracts.Authenticated(bound.actorId()))
                                        .submit());
    }

    private static ActorRef resolve(ZLinkActorCreateResult result) {
        if (result instanceof ZLinkActorCreateResult.Existing existing) {
            return existing.actor();
        }
        if (result instanceof ZLinkActorCreateResult.Created created) {
            return created.actor();
        }
        throw new IllegalStateException("Player creation was rejected.");
    }
}
// --8<-- [end:session-actor-bind]
