using SpotService.Server.Session.Handlers;
using SpotService.Shared;
using Systems.Zlink;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace SpotService.Server.Session.Spots;

internal sealed class ScenarioActorFactory : IZLinkActorFactory<ScenarioActor>
{
    public ValueTask<ScenarioActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(new ScenarioActor(context.ActorId, context));
    }
}

internal sealed class ScenarioActor(string actorId, IZLinkActorContext context) : IZLinkActor
{
    public string DisplayName { get; set; } = actorId;

    public int Seen { get; set; }
    public string ActorId { get; } = actorId;

    public IZLinkActorContext Context { get; } = context;
}

internal sealed class ScenarioEntrySpot(
    IZLinkEntrySpotContext context,
    EvidenceStore evidence) : IZLinkEntrySpot<ScenarioActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
        ScenarioActor actor,
        ZLinkMessage createRequest,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (!createRequest.IsEmpty) actor.DisplayName = createRequest.Decode<ScenarioActorCreateReq>().DisplayName;

        evidence.Add($"entry-created|rid={evidence.Rid}|actor={actor.ActorId}");
        return ValueTask.FromResult(ZLinkActorCreateResponse.Accept());
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        _ = actorId;
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept(request));
    }

    public ValueTask OnJoinedActorAsync(
        ScenarioActor actor,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"entry-joined|rid={evidence.Rid}|actor={actor.ActorId}");
        return ValueTask.CompletedTask;
    }

    public ValueTask OnLeaveActorAsync(
        ScenarioActor actor,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"entry-left|rid={evidence.Rid}|actor={actor.ActorId}");
        return ValueTask.CompletedTask;
    }

    public async ValueTask OnDisconnectActorAsync(
        ScenarioActor actor,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"entry-disconnect-started|rid={evidence.Rid}|actor={actor.ActorId}");
        if (actor.ActorId.Contains("sm-d5-race", StringComparison.Ordinal))
        {
            await Task.Delay(TimeSpan.FromSeconds(1), cancellationToken);
        }

        evidence.Add($"entry-disconnected|rid={evidence.Rid}|actor={actor.ActorId}");
        if (actor.ActorId.Contains("sm-d5-fail", StringComparison.Ordinal))
        {
            throw new InvalidOperationException("SM-D5 injected disconnect callback failure.");
        }
    }
}

internal sealed class ScenarioUserSpot(
    IZLinkSpotContext context,
    EvidenceStore evidence) : IZLinkSpot<ScenarioActor>
{
    private int _value;

    public IZLinkSpotContext Context { get; } = context;

    public ValueTask OnInitializeAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"spot-initialize|rid={evidence.Rid}|spot={Context.SpotId}");
        return ValueTask.CompletedTask;
    }

    public ValueTask OnClosingAsync(
        ZLinkSpotClosingContext context,
        CancellationToken cleanupCancellationToken)
    {
        _ = context;
        cleanupCancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"spot-closing|rid={evidence.Rid}|spot={Context.SpotId}");
        return ValueTask.CompletedTask;
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"spot-actor-admitted|rid={evidence.Rid}|spot={Context.SpotId}|actor={actorId}");
        return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept(request));
    }

    public ValueTask OnJoinedActorAsync(ScenarioActor actor, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"spot-actor-joined|rid={evidence.Rid}|spot={Context.SpotId}|actor={actor.ActorId}");
        return ValueTask.CompletedTask;
    }

    public ValueTask OnLeaveActorAsync(ScenarioActor actor, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"spot-actor-left|rid={evidence.Rid}|spot={Context.SpotId}|actor={actor.ActorId}");
        return ValueTask.CompletedTask;
    }

    public ValueTask OnDisconnectActorAsync(ScenarioActor actor, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"spot-actor-disconnected|rid={evidence.Rid}|spot={Context.SpotId}|actor={actor.ActorId}");
        return ValueTask.CompletedTask;
    }

    public ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
        Message request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"spot-created|rid={evidence.Rid}|spot={Context.SpotId}");
        return ValueTask.FromResult(ZLinkSpotCreateResponse.Accept());
    }

    public int Add(int delta)
    {
        _value += delta;
        return _value;
    }
}

internal sealed class ScenarioAlternateSpot(
    IZLinkSpotContext context) : IZLinkSpot
{
    public IZLinkSpotContext Context { get; } = context;
}
