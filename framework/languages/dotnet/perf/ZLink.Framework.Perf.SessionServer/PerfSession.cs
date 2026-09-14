using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Streams;

namespace ZLink.Framework.Perf;

// §11.1: STREAM-only receiver; no Object Server, Actor, Store or automatic discovery.
public sealed class PerfSession(IZLinkSessionContext context, Measurement measurement) : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;
    public void Configure()
    {
        Context.Handlers.AddHandler<SessionEchoHandler>(nameof(PerfEchoRequest));
        if (measurement.Config.scenario != "session-echo-only") Context.Handlers.AddHandler<SessionBindHandler>(nameof(PerfBindRequest));
    }
    public ValueTask OnConnectedAsync(CancellationToken cancellationToken) => ValueTask.CompletedTask;
    public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) => ValueTask.CompletedTask;
    public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken)
    {
        measurement.RecordDiagnostic(new InvalidOperationException($"STREAM {error.Error}: {error.Message}"));
        return ValueTask.CompletedTask;
    }
    public async ValueTask OnDispatchAsync(ZLinkSessionDispatchContext dispatch, ZLinkMessage payload, CancellationToken cancellationToken)
    {
        if (measurement.Config.scenario != "session-echo-only" && dispatch.PacketName == nameof(PerfEchoRequest))
        {
            var actor = Context.Actors.Bound.Single();
            measurement.RecordRelay();
            await actor.RelayAsync(payload, cancellationToken);
            return;
        }
        if (!await Context.Handlers.TryHandleAsync(dispatch, payload, cancellationToken))
            throw new InvalidOperationException("No typed perf session handler was registered for the packet.");
    }
}
public sealed class SessionEchoHandler(Measurement measurement) : IZLinkSessionPacketHandler<IZLinkSessionContext, PerfEchoRequest>
{
    public async ValueTask HandleAsync(IZLinkSessionContext context, ZLinkSessionDispatchContext dispatch,
        PerfEchoRequest request, CancellationToken cancellationToken)
    {
        var received = PerfClock.Now;
        measurement.HandlerEnter();
        try
        {
            measurement.ValidateRequest(request);
            var reply = PayloadPattern.Reply(request, received);
            measurement.RecordReply(request);
            await context.Client.Reply(reply).Async(cancellationToken);
            if (measurement.Phase == "setup") measurement.SetupEvidence =
                [new { kind = "typedProbeReply", source = "SessionEchoHandler.Client.Reply.Async", observedValue = request.correlationId }];
        }
        catch (Exception error) { measurement.RecordDiagnostic(error); throw; }
        finally { measurement.HandlerExit(); }
    }
}

public sealed class SessionBindHandler(Measurement measurement, IZLinkActorManager actors) : IZLinkSessionPacketHandler<IZLinkSessionContext, PerfBindRequest>
{
    public async ValueTask HandleAsync(IZLinkSessionContext context, ZLinkSessionDispatchContext dispatch,
        PerfBindRequest request, CancellationToken token)
    {
        var config = measurement.Config;
        if (request.runId != config.runId || request.cellId != config.cellId || request.clientId < 0 || request.clientId >= config.actorIds.Length)
            throw new PerfValidationException("IdentityMismatch", "Actor binding identity differs.");
        var id = config.actorIds[request.clientId];
        var result = await actors.GetOrCreate(id, "perf-actor").InMesh(config.meshName!)
            .Timeout(TimeSpan.FromMilliseconds(config.workload.setupTimeoutMs)).Async(token);
        var actor = result switch
        {
            ZLinkActorCreateResult.Created created => created.Actor,
            ZLinkActorCreateResult.Existing existing => existing.Actor,
            _ => throw new PerfValidationException("PreparationRejected", "Actor create rejected.")
        };
        var bound = await context.Actors.BindOrGetAsync(actor, token);
        await context.Client.Reply(new PerfBindReply(bound.ActorId, true)).Async(token);
        measurement.SetupEvidence = [new { kind = "publicActorCreatedAndSessionBound", source = "GetOrCreate.Async + Actors.BindOrGetAsync", observedValue = id }];
    }
}
