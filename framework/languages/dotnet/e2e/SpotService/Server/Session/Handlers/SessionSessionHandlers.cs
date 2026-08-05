using SpotService.Server.Session.Spots;
using SpotService.Shared;
using Systems.Zlink;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Streams;

namespace SpotService.Server.Session.Handlers;

internal sealed class ScenarioSession(
    IZLinkSessionContext context,
    EvidenceStore evidence) : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddHandler<AuthSessionHandler>();
        Context.Handlers.AddHandler<MultiBindSessionHandler>();
        Context.Handlers.AddHandler<StaleBindingProbeSessionHandler>();
        Context.Handlers.AddHandler<UserSpotAuthSessionHandler>();
        Context.Handlers.AddHandler<NotifyBoundActorDisconnectedSessionHandler>();
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

internal sealed class NotifyBoundActorDisconnectedSessionHandler
    : IZLinkSessionPacketHandler<IZLinkSessionContext, NotifyBoundActorDisconnectedReq>
{
    public async ValueTask HandleAsync(
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        NotifyBoundActorDisconnectedReq request,
        CancellationToken cancellationToken)
    {
        _ = dispatch;
        var actor = context.Actors.Find(request.ActorId)
                    ?? throw new InvalidOperationException(
                        $"Actor route not found: {request.ActorId}");
        await actor.NotifyDisconnectedAsync(cancellationToken);
        await context.Client
            .Reply(new NotifyBoundActorDisconnectedRes(request.ActorId, true))
            .Async(cancellationToken);
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
        evidence.Add(
            $"actor-bound|rid={ensured.NodeRid}|actor={ensured.ActorId}"
            + $"|generation={ensured.Generation}|session={context.SessionId}");
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
    EvidenceStore evidence,
    SessionBindingProbeStore bindingProbes)
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
            var bound = await context.Actors.BindAsync(
                new ActorRef(
                    ensured.ActorId,
                    ensured.Generation,
                    SpotServiceNames.SpotChannel,
                    RoutingId.From(ensured.NodeRid)),
                cancellationToken);
            bindingProbes.Record(context.SessionId, bound);
        }

        await context.Client.Reply(new MultiBindRes(context.Actors.Bound.Count))
            .Async(cancellationToken);
    }
}

internal sealed class StaleBindingProbeSessionHandler(
    SessionBindingProbeStore bindingProbes)
    : IZLinkSessionPacketHandler<IZLinkSessionContext, StaleBindingProbeReq>
{
    public async ValueTask HandleAsync(
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        StaleBindingProbeReq request,
        CancellationToken cancellationToken)
    {
        _ = dispatch;
        var preserved = bindingProbes.Require(context.SessionId, request.ActorId);
        var relayRejected = false;
        var errorKind = string.Empty;
        try
        {
            await preserved.RelayAsync(
                ZLinkMessage.From(new ActorPingReq(request.Value)),
                cancellationToken);
        }
        catch (ZLinkFrameworkException error)
        {
            relayRejected = true;
            errorKind = error.Kind.ToString();
        }

        await preserved.NotifyDisconnectedAsync(cancellationToken);
        await context.Client.Reply(new StaleBindingProbeRes(
                request.ActorId,
                relayRejected,
                errorKind,
                DisconnectCompleted: true))
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
