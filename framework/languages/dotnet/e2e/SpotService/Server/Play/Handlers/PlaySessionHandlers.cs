using SpotService.Server.Play.Spots;
using SpotService.Shared;
using Systems.Zlink;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Streams;

namespace SpotService.Server.Play.Handlers;

internal sealed class ScenarioSession(
    IZLinkSessionContext context,
    EvidenceStore evidence) : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddHandler<AuthSessionHandler>();
        Context.Handlers.AddHandler<MultiBindSessionHandler>();
        Context.Handlers.AddHandler<UserSpotAuthSessionHandler>();
    }

    public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"session-connected|rid={evidence.Rid}|session={Context.SessionId}");
        return ValueTask.CompletedTask;
    }

    public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"session-disconnected|rid={evidence.Rid}|session={Context.SessionId}");
        return ValueTask.CompletedTask;
    }

    public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"session-error|rid={evidence.Rid}|error={error}");
        return ValueTask.CompletedTask;
    }

    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        if (await Context.Handlers.TryHandleAsync(dispatch, payload, cancellationToken)) return;

        var actorId = dispatch.Metadata.Find(SpotServiceNames.ActorIdMetadata);
        var actor = string.IsNullOrWhiteSpace(actorId)
            ? RequireSingleBoundActor()
            : Context.Actors.Find(actorId);
        if (actor is null)
        {
            throw new InvalidOperationException($"Actor route not found: {actorId}");
        }

        await actor.RelayAsync(payload, cancellationToken);
    }

    private IZLinkSessionActor RequireSingleBoundActor()
    {
        return Context.Actors.Bound.Count switch
        {
            1 => Context.Actors.Bound.Single(),
            0 => throw new InvalidOperationException("No actor is bound."),
            _ => throw new InvalidOperationException("ActorRouteNotFound: actor-id metadata is required.")
        };
    }
}

internal sealed class AuthSessionHandler(
    IZLinkActorManager actors,
    EvidenceStore evidence)
    : IZLinkSessionPacketHandler<IZLinkSessionContext, AuthReq>
{
    public async ValueTask HandleAsync(
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        AuthReq request,
        CancellationToken cancellationToken)
    {
        _ = dispatch;
        var ensured = await EnsureActorAsync(
            actors,
            evidence,
            request.ActorId,
            request.DisplayName,
            cancellationToken);
        await context.Actors.BindAsync(
            new ActorRef(
                ensured.ActorId,
                ensured.Generation,
                SpotServiceNames.SpotChannel,
                RoutingId.From(ensured.NodeRid)),
            cancellationToken);
        await context.Client.Reply(new AuthRes(ensured.ActorId, ensured.NodeRid))
            .Async(cancellationToken);
    }

    internal static async ValueTask<EnsureActorRes> EnsureActorAsync(
        IZLinkActorManager actors,
        EvidenceStore evidence,
        string actorId,
        string displayName,
        CancellationToken cancellationToken)
    {
        var actor = await actors
            .GetOrCreate(actorId, SpotServiceNames.ActorType)
            .InMesh(SpotServiceNames.SpotChannel)
            .Request(new ScenarioActorCreateReq(displayName))
            .Async(cancellationToken) switch
        {
            ZLinkActorCreateResult.Existing value => value.Actor,
            ZLinkActorCreateResult.Created value => value.Actor,
            _ => throw new InvalidOperationException("Actor creation was rejected.")
        };

        evidence.Add($"ensure-actor|rid={actor.NodeRid}|actor={actorId}");
        evidence.Add($"entry-joined|rid={actor.NodeRid}|actor={actorId}");
        return new EnsureActorRes(
            actor.ActorId,
            actor.NodeRid.ToString(),
            actor.ObjectGeneration);
    }
}

internal sealed class MultiBindSessionHandler(
    IZLinkActorManager actors,
    EvidenceStore evidence)
    : IZLinkSessionPacketHandler<IZLinkSessionContext, MultiBindReq>
{
    public async ValueTask HandleAsync(
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        MultiBindReq request,
        CancellationToken cancellationToken)
    {
        _ = dispatch;
        foreach (var actorId in new[] { request.FirstActorId, request.SecondActorId })
        {
            var ensured = await AuthSessionHandler.EnsureActorAsync(
                actors,
                evidence,
                actorId,
                actorId,
                cancellationToken);
            await context.Actors.BindAsync(
                new ActorRef(
                    ensured.ActorId,
                    ensured.Generation,
                    SpotServiceNames.SpotChannel,
                    RoutingId.From(ensured.NodeRid)),
                cancellationToken);
        }

        await context.Client.Reply(new MultiBindRes(context.Actors.Bound.Count))
            .Async(cancellationToken);
    }
}

internal sealed class UserSpotAuthSessionHandler(
    IZLinkActorManager actors,
    EvidenceStore evidence)
    : IZLinkSessionPacketHandler<IZLinkSessionContext, UserSpotAuthReq>
{
    public async ValueTask HandleAsync(
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        UserSpotAuthReq request,
        CancellationToken cancellationToken)
    {
        _ = dispatch;
        var ensured = await EnsureActorAsync(actors, evidence, request, cancellationToken);
        await context.Actors.BindAsync(
            new ActorRef(
                ensured.ActorId,
                ensured.Generation,
                SpotServiceNames.SpotChannel,
                RoutingId.From(ensured.NodeRid)),
            cancellationToken);
        await context.Client.Reply(new AuthRes(ensured.ActorId, ensured.NodeRid))
            .Async(cancellationToken);
    }

    private static async ValueTask<EnsureActorRes> EnsureActorAsync(
        IZLinkActorManager actors,
        EvidenceStore evidence,
        UserSpotAuthReq request,
        CancellationToken cancellationToken)
    {
        var actor = await actors
            .GetOrCreate(request.ActorId, SpotServiceNames.ActorType)
            .InMesh(SpotServiceNames.SpotChannel)
            .Request(new ScenarioActorCreateReq(request.SpotRid))
            .Async(cancellationToken) switch
        {
            ZLinkActorCreateResult.Existing value => value.Actor,
            ZLinkActorCreateResult.Created value => value.Actor,
            _ => throw new InvalidOperationException("Actor creation was rejected.")
        };

        evidence.Add(
            $"ensure-user-spot-actor|rid={evidence.Rid}|spot={request.SpotRid}"
            + $"|actor={request.ActorId}");
        return new EnsureActorRes(
            actor.ActorId,
            actor.NodeRid.ToString(),
            actor.ObjectGeneration);
    }
}
