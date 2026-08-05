using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Streams;
using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Server.Gateway.Infrastructure.ZLink.Sessions;

/// <summary>
/// One browser connection. It terminates the WebSocket, binds the session to the player's
/// actor, and relays everything else to it.
///
/// The binding is what keeps the connection alive across a zone change: when the actor
/// relocates to another node, the bound session follows it, so the client never
/// reconnects (ZW-B2).
/// </summary>
public sealed class PlayerSession(
    IZLinkSessionContext context,
    PlayerSessionBinder binder,
    RelocationProbeService relocationProbes,
    IZLinkActorClient actors,
    ILogger<PlayerSession> logger) : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
    {
        logger.LogInformation("client connected. session={SessionId}", Context.SessionId);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
    {
        // Physical disconnect is delivered to each exact Actor binding by Framework after
        // this callback. The session callback must not send a second logical notification:
        // doing both races the relocation route fence and can retain a stale push route.
        _ = cancellationToken;
        logger.LogInformation("client disconnected. session={SessionId}", Context.SessionId);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken)
    {
        logger.LogWarning(
            "stream error. session={SessionId}, code={Code}, message={Message}",
            Context.SessionId,
            error.Error,
            error.Message);
        return ValueTask.CompletedTask;
    }

    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        logger.LogInformation(
            "session packet received. session={SessionId}, packet={PacketName}",
            Context.SessionId,
            dispatch.PacketName);
        if (dispatch.PacketName == nameof(RelocationPairReq))
        {
            await Context.Client
                .Reply(await relocationProbes.SelectPairAsync(cancellationToken))
                .Async(cancellationToken);
            return;
        }

        if (dispatch.PacketName == nameof(ActorLocationProbeReq))
        {
            var request = payload.Decode<ActorLocationProbeReq>();
            await Context.Client
                .Reply(await relocationProbes.FindActorAsync(request.ActorId, cancellationToken))
                .Async(cancellationToken);
            return;
        }

        if (dispatch.PacketName == nameof(MessageFollowProbeReq))
        {
            var request = payload.Decode<MessageFollowProbeReq>();
            if (dispatch.CanReply)
            {
                var reply = await actors
                    .RequestToActor(request.ActorId, request)
                    .Async<MessageFollowProbeRes>(cancellationToken);
                await Context.Client.Reply(reply).Async(cancellationToken);
            }
            else
            {
                await actors
                    .SendToActor(request.ActorId, request)
                    .Async(cancellationToken);
            }
            return;
        }

        if (Context.Actors.Bound.Count == 0)
        {
            if (dispatch.PacketName != nameof(JoinWorldReq))
                throw new InvalidOperationException(
                    $"Client must join the world before sending '{dispatch.PacketName}'.");

            await binder.BindAsync(Context, payload.Decode<JoinWorldReq>().PlayerId, cancellationToken);
        }

        var actor = Context.Actors.Bound.Single();

        // Everything, including the join itself, is relayed to the actor. The relay is what
        // tells the node hosting the actor where to push: binding the session registers the
        // route on this node only, and the actor's node learns it from the first relayed
        // packet. Answer the join without relaying it and a player who never moves would
        // sit in the world receiving nothing.
        await actor.RelayAsync(payload, cancellationToken);
    }
}

/// <summary>
/// Finds the player by its global ActorId and lets the framework select an eligible owner.
/// The returned ActorRef is used directly; application code never reconstructs an owner route.
/// </summary>
public sealed class PlayerSessionBinder(
    IZLinkActorManager actors,
    ILogger<PlayerSessionBinder> logger)
{
    public async ValueTask BindAsync(
        IZLinkSessionContext context,
        string playerId,
        CancellationToken cancellationToken)
    {
        var actorRef = await actors
            .GetOrCreate(playerId, ZoneWorldNames.PlayerActorType)
            .InMesh(ZoneWorldNames.MeshName)
            .Request(ZLinkMessage.Empty)
            .Async(cancellationToken) switch
        {
            ZLinkActorCreateResult.Existing value => value.Actor,
            ZLinkActorCreateResult.Created value => value.Actor,
            _ => throw new InvalidOperationException("Player Actor creation was rejected.")
        };

        await context.Actors.BindOrGetAsync(actorRef, cancellationToken);
        logger.LogInformation("session bound to player actor. player={PlayerId}", playerId);
    }
}
