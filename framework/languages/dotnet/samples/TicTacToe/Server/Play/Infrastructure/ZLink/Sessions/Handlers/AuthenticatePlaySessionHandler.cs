using TicTacToe.Server.Configuration;
using TicTacToe.Shared.Contracts;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Streams;

namespace TicTacToe.Server.Play.Infrastructure.ZLink.Sessions.Handlers;

// --8<-- [start:doc-session-auth]
internal sealed class AuthenticatePlaySessionHandler(
    IZLinkActorManager actors,
    IZLinkRouteClient channels,
    ILogger<AuthenticatePlaySessionHandler> logger)
    : IZLinkSessionPacketHandler<IZLinkSessionContext, AuthenticateReq>
{
    public async ValueTask HandleAsync(
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        AuthenticateReq authenticate,
        CancellationToken cancellationToken)
    {
        logger.LogInformation(
            "play stream: authenticate requested. sessionId={SessionId}",
            context.SessionId);

        var accessToken = authenticate.AccessToken.Trim();
        if (string.IsNullOrWhiteSpace(accessToken))
            throw new InvalidOperationException("Authentication token is empty.");

        var authenticated = await channels.RequestToChannel(
                SampleChannels.Api,
                new AuthenticatePlayerReq(accessToken))
            .Async<AuthenticatePlayerRes>(cancellationToken);

        logger.LogInformation(
            "play stream: authenticate accepted. sessionId={SessionId}, player={ActorId}",
            context.SessionId,
            authenticated.Player.ActorId);

        await EnsureActorBoundAsync(
            context,
            authenticated.Player,
            cancellationToken);

        await context.Client.Reply(new AuthenticateRes(authenticated.Player))
            .Async(cancellationToken);
    }

    private async ValueTask EnsureActorBoundAsync(
        IZLinkSessionContext context,
        PlayerInfo player,
        CancellationToken cancellationToken)
    {
        logger.LogInformation(
            "play stream: creating actor before dispatch. sessionId={SessionId}, actor={ActorId}",
            context.SessionId,
            player.ActorId);
        var result = await actors.GetOrCreate(player.ActorId, SampleTypes.PlayerActor)
            .Request(new PlayerActorCreateReq(player)).Async(cancellationToken);
        var playerActor = result switch
        {
            ZLinkActorCreateResult.Existing value => value.Actor,
            ZLinkActorCreateResult.Created value => value.Actor,
            _ => throw new InvalidOperationException("Player Actor creation was rejected.")
        };
        logger.LogInformation(
            "play stream: binding actor to session. sessionId={SessionId}, actor={ActorId}",
            context.SessionId,
            player.ActorId);
        var boundActor = await context.Actors.BindOrGetAsync(
            playerActor,
            cancellationToken);

        // ActorRef equality covers the actor id, object generation, mesh and owner
        // route. Keep that exact-identity check on the server; AuthenticateRes only
        // returns the public PlayerInfo payload.
        if (boundActor.Ref != playerActor)
            throw new InvalidOperationException(
                $"Bound ActorRef does not match the resolved ActorRef for '{player.ActorId}'.");

        logger.LogInformation(
            "play stream: actor bound to session. sessionId={SessionId}, actor={ActorId}",
            context.SessionId,
            boundActor.ActorId);

        if (result is ZLinkActorCreateResult.Existing)
            logger.LogInformation(
                "tictactoe-lifecycle actor-bound actor={ActorId}",
                boundActor.ActorId);
    }
}
// --8<-- [end:doc-session-auth]
