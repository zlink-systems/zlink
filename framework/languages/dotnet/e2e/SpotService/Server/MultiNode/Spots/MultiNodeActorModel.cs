using SpotService.Server.MultiNode.Handlers;
using SpotService.Shared;
using Systems.Zlink;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace SpotService.Server.MultiNode.Spots;

internal sealed class ScenarioActorFactory(EvidenceStore evidence) : IZLinkActorFactory<ScenarioActor>
{
    public ValueTask<ScenarioActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(new ScenarioActor(context.ActorId, context, evidence));
    }
}

internal sealed class ScenarioActor(
    string actorId,
    IZLinkActorContext context,
    EvidenceStore evidence) : IZLinkActor
{
    private readonly System.Collections.Concurrent.ConcurrentQueue<(string TargetSpotId, string Marker)>
        _pendingJoins = new();

    public string DisplayName { get; set; } = actorId;

    public int Seen { get; set; }
    public string ActorId { get; } = actorId;

    public IZLinkActorContext Context { get; } = context;

    public void RecordDeferredJoin(string targetSpotId, string marker)
    {
        _pendingJoins.Enqueue((targetSpotId, marker));
    }

    public ValueTask OnJoinCompletedAsync(
        ZLinkActorJoinCompletion completion,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (!_pendingJoins.TryDequeue(out var pending))
            throw new InvalidOperationException(
                $"Actor '{ActorId}' received Join completion without a pending E2E request.");

        var accepted = completion is ZLinkActorJoinCompletion.Accepted;
        var completedActor = completion is ZLinkActorJoinCompletion.Accepted value
            ? value.Actor
            : default;
        evidence.Add(
            $"spot-only-actor-join-completed|rid={evidence.Rid}|actor={ActorId}"
            + $"|target={pending.TargetSpotId}|accepted={accepted}"
            + $"|generation={completedActor.ObjectGeneration}|marker={pending.Marker}");
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

    public ValueTask OnDisconnectActorAsync(
        ScenarioActor actor,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"entry-disconnected|rid={evidence.Rid}|actor={actor.ActorId}");
        return ValueTask.CompletedTask;
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
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        _ = request;
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

internal sealed class SpotOnlyUserSpot(
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

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        _ = request;
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"spot-actor-admitted|rid={evidence.Rid}|spot={Context.SpotId}|actor={actorId}");
        return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());
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

    public async ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"spot-created|rid={evidence.Rid}|spot={Context.SpotId}");
        if (!request.IsEmpty)
        {
            var command = request.Decode<SpotOnlyMeshReq>();
            var reply = await Context.Outbound
                .RequestToSpot(command.TargetSpotRid, new StateReq("add", 7))
                .Async<StateRes>(cancellationToken);
            await Context.Outbound.SendToSpot(
                    command.TargetSpotRid,
                    new StateMsg($"sm-f6-send-{command.Marker}"))
                .Async(cancellationToken);
            evidence.Add(
                $"spot-only-request|rid={evidence.Rid}|source={Context.SpotId}"
                + $"|target={command.TargetSpotRid}|value={reply.Value}|marker={command.Marker}");
        }

        return ZLinkSpotCreateResponse.Accept();
    }

    public int Add(int delta)
    {
        _value += delta;
        return _value;
    }
}
