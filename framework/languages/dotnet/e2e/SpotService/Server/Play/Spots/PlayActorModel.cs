using SpotService.Server.Play.Handlers;
using SpotService.Shared;
using Systems.Zlink;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace SpotService.Server.Play.Spots;

internal sealed class ScenarioActorFactory(EvidenceStore evidence) : IZLinkActorFactory<ScenarioActor>
{
    public ValueTask<ScenarioActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"actor-factory|rid={evidence.Rid}|actor={context.ActorId}");
        return ValueTask.FromResult(new ScenarioActor(context.ActorId, context, evidence));
    }
}

internal sealed class ScenarioActor(
    string actorId,
    IZLinkActorContext context,
    EvidenceStore evidence) : IZLinkActor
{
    public string DisplayName { get; set; } = actorId;

    public int Seen { get; set; }
    public string ActorId { get; } = actorId;

    public IZLinkActorContext Context { get; } = context;

    public ValueTask OnJoinCompletedAsync(
        ZLinkActorJoinCompletion completion,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        switch (completion)
        {
            case ZLinkActorJoinCompletion.Accepted accepted:
                evidence.Add(
                    $"actor-join-completed|rid={evidence.Rid}|actor={ActorId}"
                    + $"|spot={Context.SpotId}|generation={accepted.Actor.ObjectGeneration}");
                break;
            case ZLinkActorJoinCompletion.Rejected:
                evidence.Add(
                    $"actor-join-rejected|rid={evidence.Rid}|actor={ActorId}"
                    + $"|spot={Context.SpotId}");
                break;
            case ZLinkActorJoinCompletion.Failed failed:
                evidence.Add(
                    $"actor-join-failed|rid={evidence.Rid}|actor={ActorId}"
                    + $"|spot={Context.SpotId}|kind={failed.Kind}");
                break;
        }

        return ValueTask.CompletedTask;
    }
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
        if (!request.IsEmpty)
        {
            var joinAdmission = request.Decode<JoinAdmittedUserSpotActorReq>();
            if (!joinAdmission.Allow)
            {
                evidence.Add(
                    $"spot-actor-join-rejected|rid={evidence.Rid}|spot={Context.SpotId}"
                    + $"|actor={actorId}|reason={joinAdmission.Reason}");
                return ValueTask.FromResult(ZLinkSpotActorJoinResult.Reject(joinAdmission));
            }

            evidence.Add(
                $"spot-actor-join-admitted|rid={evidence.Rid}|spot={Context.SpotId}"
                + $"|actor={actorId}|reason={joinAdmission.Reason}");
        }

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

internal sealed class ScenarioWeightCapacitySpot(
    IZLinkSpotContext context) : IZLinkSpot
{
    public IZLinkSpotContext Context { get; } = context;
}
