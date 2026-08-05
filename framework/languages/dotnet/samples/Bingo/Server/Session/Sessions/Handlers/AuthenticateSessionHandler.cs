using Bingo.Server.Configuration;
using Bingo.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Streams;

namespace Bingo.Server.Session.Sessions.Handlers;

internal sealed class AuthenticateBingoSessionHandler(
    IZLinkActorManager actors,
    IZLinkRouteClient channels,
    ILogger<AuthenticateBingoSessionHandler> logger)
    : IZLinkSessionPacketHandler<IZLinkSessionContext, AuthenticateReq>
{
    public async ValueTask HandleAsync(
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        AuthenticateReq request,
        CancellationToken cancellationToken)
    {
        var authenticated = await channels.RequestToChannel(SampleNames.ApiChannel,
                new AuthenticatePlayerReq { AccessToken = request.AccessToken })
            .Async<AuthenticatePlayerRes>(cancellationToken);

        if (!authenticated.Accepted
            || string.IsNullOrWhiteSpace(authenticated.ActorId)
            || string.IsNullOrWhiteSpace(authenticated.DisplayName))
            throw new InvalidOperationException(authenticated.Reason ?? "Player authentication failed.");

        var actor = (await actors
                .GetOrCreate(authenticated.ActorId, SampleNames.PlayerActorType)
                .InMesh(SampleNames.PlayMeshName)
                .Request(new EnsurePlayerActorReq
                {
                    ActorId = authenticated.ActorId,
                    DisplayName = authenticated.DisplayName
                })
                .Async(cancellationToken)) switch
        {
            ZLinkActorCreateResult.Existing value => value.Actor,
            ZLinkActorCreateResult.Created value => value.Actor,
            _ => throw new InvalidOperationException("Player Actor creation was rejected.")
        };

        var boundActor = await context.Actors.BindOrGetAsync(
            actor,
            cancellationToken);
        logger.LogInformation(
            "bingo session: bound player={ActorId} session={SessionId}",
            boundActor.ActorId,
            context.SessionId);

        await context.Client.Reply(new AuthenticateRes
            {
                ActorId = authenticated.ActorId,
                DisplayName = authenticated.DisplayName
            })
            .Async(cancellationToken);
    }
}
