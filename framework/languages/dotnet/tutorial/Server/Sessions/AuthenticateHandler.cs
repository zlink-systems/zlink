using Tutorial.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Streams;

namespace Tutorial.Server.Sessions;

// --8<-- [start:session-actor-bind]
// Ties this connection to one player. After this, packets without a session
// handler reach that player, and the player can push to this connection.
public sealed class AuthenticateHandler(IZLinkActorManager players)
    : IZLinkSessionPacketHandler<IZLinkSessionContext, Authenticate>
{
    public async ValueTask HandleAsync(
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        Authenticate message,
        CancellationToken cancellationToken
    )
    {
        // A returning client finds its existing player rather than a new one.
        var result = await players
            .GetOrCreate(message.PlayerId, "player")
            .InMesh("game")
            .Request(new CreatePlayer(message.PlayerId))
            .Async(cancellationToken);

        var player = result switch
        {
            ZLinkActorCreateResult.Existing value => value.Actor,
            ZLinkActorCreateResult.Created value => value.Actor,
            _ => throw new InvalidOperationException("Player creation was rejected."),
        };

        var bound = await context.Actors.BindOrGetAsync(player, cancellationToken);

        await context.Client.Reply(new Authenticated(bound.ActorId)).Async(cancellationToken);
    }
}
// --8<-- [end:session-actor-bind]
