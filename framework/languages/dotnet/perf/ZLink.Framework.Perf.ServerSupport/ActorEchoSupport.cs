using Microsoft.Extensions.Hosting;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Streams;

namespace ZLink.Framework.Perf;

// Actor echo objects shared by the CS (§10.1, §10.2) and Actor (§10.9, §10.10) Object Servers.
// The Actor holds no state: every measured call is the typed echo of the Entry Spot's Actor handler.
public sealed class PerfActor(IZLinkActorContext context) : IZLinkActor
{
    public IZLinkActorContext Context { get; } = context;
}

public sealed class PerfActorFactory : IZLinkActorFactory<PerfActor>
{
    public ValueTask<PerfActor> CreateAsync(IZLinkActorContext context, CancellationToken cancellationToken = default) =>
        ValueTask.FromResult(new PerfActor(context));
    async ValueTask<IZLinkActor> IZLinkActorFactory.CreateAsync(IZLinkActorContext context, CancellationToken cancellationToken) =>
        await CreateAsync(context, cancellationToken);
}

public static class PerfActorType
{
    public const string Name = "perf-actor";
    // Actors are created as Entry Spot members and are never moved (perf never measures relocation).
    public static IZLinkMeshObjectServerBuilder AddPerfActors(this IZLinkMeshObjectServerBuilder objects) => objects
        .AddEntrySpot<PerfEntrySpot>()
        .AddActorFactory<PerfActor, PerfActorFactory>(Name, factory => factory.DisableRelocation());
}

// The request cells answer with the typed reply; the send-send cell answers with a public Channel send (§10.10).
public sealed class PerfEntrySpot(IZLinkEntrySpotContext context, RoleConfig config) : IZLinkEntrySpot<PerfActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;
    public void Configure()
    {
        if (config.mode == "send-send") Context.Handlers.AddActorPacket<ActorEchoSendHandler, PerfActor>(nameof(PerfEchoRequest));
        else Context.Handlers.AddActorPacket<ActorEchoRequestHandler, PerfActor>(nameof(PerfEchoRequest));
    }
    public ValueTask OnJoinedActorAsync(PerfActor actor, CancellationToken cancellationToken) => ValueTask.CompletedTask;
    public ValueTask OnLeaveActorAsync(PerfActor actor, CancellationToken cancellationToken) => ValueTask.CompletedTask;
}

public sealed class ActorEchoRequestHandler(Measurement measurement)
    : IZLinkEntrySpotActorRequestHandler<PerfEntrySpot, PerfActor, PerfEchoRequest, PerfEchoReply>
{
    public ValueTask<PerfEchoReply> HandleAsync(PerfEntrySpot spot, PerfActor actor, IZLinkMessageContext context,
        PerfEchoRequest request, CancellationToken cancellationToken)
    {
        var received = PerfClock.Now;
        measurement.HandlerEnter();
        try
        {
            measurement.ValidateRequest(request);
            var reply = PayloadPattern.Reply(request, received);
            measurement.RecordReply(request);
            if (measurement.Phase == "setup") measurement.SetupEvidence = [new { kind = "typedProbeReply",
                source = "IZLinkEntrySpotActorRequestHandler<PerfEntrySpot,PerfActor,PerfEchoRequest,PerfEchoReply>", observedValue = request.correlationId }];
            return ValueTask.FromResult(reply);
        }
        catch (Exception error) { measurement.RecordDiagnostic(error); throw; }
        finally { measurement.HandlerExit(); }
    }
}

public sealed class ActorEchoSendHandler(Measurement measurement, RoleConfig config, IZLinkRouteClient route)
    : IZLinkEntrySpotActorSendHandler<PerfEntrySpot, PerfActor, PerfEchoRequest>
{
    public async ValueTask HandleAsync(PerfEntrySpot spot, PerfActor actor, IZLinkMessageContext context,
        PerfEchoRequest request, CancellationToken cancellationToken)
    {
        var received = PerfClock.Now;
        measurement.HandlerEnter();
        try
        {
            measurement.ValidateRequest(request, returnChannel: config.channelName);
            var reply = PayloadPattern.Reply(request, received);
            measurement.RecordApplicationCall(request, "send");
            await route.SendToChannel(request.returnChannel!, reply).Async(cancellationToken);
            if (measurement.Phase == "setup") measurement.SetupEvidence = [new { kind = "typedProbeReply",
                source = "IZLinkRouteClient.SendToChannel(returnChannel).Async", observedValue = request.correlationId }];
        }
        catch (Exception error) { measurement.RecordDiagnostic(error); throw; }
        finally { measurement.HandlerExit(); }
    }
}

// The Actor role's objectsReady (§16.1): the Actors this process hosts, read from the public RouteMesh placement
// status by a background poll during setup, never from inside a handler turn.
public sealed class ActorPlacementWatcher(RoleConfig config, Measurement measurement, ObjectsReadiness readiness,
    IZLinkRouteMeshRuntime mesh) : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        while (!stoppingToken.IsCancellationRequested && measurement.Phase == "setup")
        {
            var placement = mesh.GetStatus(config.meshName!).Placement;
            readiness.Set(placement.IsAvailable && placement.ActiveActorCount > 0, "No Actor is active on this Object Server.",
                [new { kind = "actorPlacement", source = "IZLinkRouteMeshRuntime.GetStatus.Placement",
                    observedValue = new { placement.IsAvailable, placement.ActiveActorCount, expectedActors = config.actorIds.Length } }]);
            await Task.Delay(100, stoppingToken).ConfigureAwait(false);
        }
    }
}

// The Session role's create and bind (§10.1, §10.2 preparation): the connector's setup probe names its clientId,
// which selects the Actor ID of that connector; the Actor is created through the public manager and bound to the
// session before the probe itself is relayed. Setup latencies are kept apart from the measured operations.
public sealed class SessionActorSetup(RoleConfig config, Measurement measurement, ObjectsReadiness readiness,
    IZLinkActorManager actors, IZLinkRouteMeshRuntime mesh)
{
    private readonly object gate = new();
    private long created, existing, bound, failed;
    private long createNs, createMaxNs, bindNs, bindMaxNs;
    // Public status shows when the Actor node is a ready peer; every session shares this one wait, and the create
    // itself is never retried.
    private readonly Lazy<Task> peer = new(() => WaitForPeerAsync(config, mesh));
    private static async Task WaitForPeerAsync(RoleConfig config, IZLinkRouteMeshRuntime mesh)
    {
        using var timeout = new CancellationTokenSource(config.workload.setupTimeoutMs);
        while (config.objectRole == "ObjectClient" && mesh.GetStatus(config.meshName!).ReadyPeerCount == 0)
            await Task.Delay(10, timeout.Token);
    }

    public async ValueTask<IZLinkSessionActor> PrepareAsync(IZLinkSessionContext session, ZLinkMessage probe, CancellationToken cancellationToken)
    {
        if (measurement.Phase != "setup") throw new InvalidOperationException("Actors are created and bound during setup only.");
        try
        {
            var request = probe.Decode<PerfEchoRequest>();
            if (request.clientId < 0 || request.clientId >= config.actorIds.Length)
                throw new PerfValidationException("IdentityMismatch", "clientId has no Actor ID in this cell.");
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            timeout.CancelAfter(config.workload.setupTimeoutMs);
            await peer.Value.WaitAsync(timeout.Token);
            var createStarted = PerfClock.Now;
            var result = await actors.GetOrCreate(config.actorIds[request.clientId], PerfActorType.Name)
                .InMesh(config.meshName!).Timeout(TimeSpan.FromMilliseconds(config.workload.setupTimeoutMs)).Async(timeout.Token);
            var actor = result switch
            {
                ZLinkActorCreateResult.Created value => value.Actor,
                ZLinkActorCreateResult.Existing value => value.Actor,
                _ => throw new InvalidOperationException("Actor creation was rejected.")
            };
            var bindStarted = PerfClock.Now;
            var binding = await session.Actors.BindOrGetAsync(actor, timeout.Token);
            var done = PerfClock.Now;
            Record(result is ZLinkActorCreateResult.Created, bindStarted - createStarted, done - bindStarted);
            return binding;
        }
        catch (Exception error)
        {
            measurement.RecordDiagnostic(error);
            lock (gate) failed++;
            Publish();
            throw;
        }
    }

    private void Record(bool wasCreated, long create, long bind)
    {
        lock (gate)
        {
            if (wasCreated) created++; else existing++;
            bound++;
            createNs += create; createMaxNs = Math.Max(createMaxNs, create);
            bindNs += bind; bindMaxNs = Math.Max(bindMaxNs, bind);
        }
        Publish();
    }

    private void Publish()
    {
        lock (gate)
        {
            readiness.Set(bound > 0 && failed == 0, failed > 0 ? "Actor create or bind failed." : "No Actor is bound to a session yet.",
                [new { kind = "actorCreateAndBind", source = "IZLinkActorManager.GetOrCreate + IZLinkSessionActors.BindOrGetAsync",
                    observedValue = new { created, existing, bound, failed, expectedActors = config.actorIds.Length,
                        createMeanMs = bound == 0 ? 0 : createNs / 1e6 / bound, createMaxMs = createMaxNs / 1e6,
                        bindMeanMs = bound == 0 ? 0 : bindNs / 1e6 / bound, bindMaxMs = bindMaxNs / 1e6 } }]);
            // The Session role has no typed reply of its own: its setup probe is the admitted relay of a bound Actor.
            if (bound > 0) measurement.SetupEvidence = [new { kind = "relayAdmission", source = "IZLinkSessionActor.RelayAsync",
                observedValue = new { bound } }];
        }
    }
}

// §10.1/§10.2: the session relays every packet to the Actor bound to it; the Actor handler's return value is
// the reply of the original STREAM request (Session binding §5), so the session writes no reply.
public sealed class PerfActorRelaySession(IZLinkSessionContext context, Measurement measurement, SessionActorSetup setup) : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;
    private IZLinkSessionActor? binding;
    public ValueTask OnConnectedAsync(CancellationToken cancellationToken) => ValueTask.CompletedTask;
    public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) => ValueTask.CompletedTask;
    public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken)
    {
        measurement.RecordDiagnostic(new InvalidOperationException($"STREAM {error.Error}: {error.Message}"));
        return ValueTask.CompletedTask;
    }
    public async ValueTask OnDispatchAsync(ZLinkSessionDispatchContext dispatch, ZLinkMessage payload, CancellationToken cancellationToken)
    {
        var actor = dispatch.Actor ?? binding ?? (binding = await setup.PrepareAsync(Context, payload, cancellationToken));
        try { await actor.RelayAsync(payload, cancellationToken); }
        catch (Exception error) { measurement.RecordDiagnostic(error); throw; }
    }
}
