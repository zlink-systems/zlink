using System.Collections.Concurrent;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace ZLink.Framework.Perf;

public sealed class PerfScenario(IServiceProvider services, Measurement measurement)
{
    private readonly RoleConfig config = measurement.Config;
    private readonly ConcurrentDictionary<(int, ulong), Correlation> correlations = new();
    private long[] sequences = [];
    private long publishSequence;
    private sealed class Correlation(PerfEchoRequest request, long started, bool setupProbe)
    {
        public readonly int ClientId = request.clientId;
        public readonly ulong Sequence = DecimalText.U64(request.sequence);
        public readonly bool Warmup = request.phase == "warmup";
        public readonly bool SetupProbe = setupProbe;
        public long Started = started;
        public TaskCompletionSource<PerfEchoReply?>? Completion = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public bool OperationStarted;
        public int Closed; // 1 success; 2 first-send failure or expiry. First terminal wins.
    }
    public static void Configure(IZLinkFrameworkOptions options, RoleConfig config)
    {
        if (config.mode == "publish")
        {
            var channel = options.AddFanoutChannel(config.channelName!);
            if (config.source) channel.EnablePublisher(config.fanoutEndpoint ?? config.listenerEndpoint!);
            else channel.EnableSubscriber().AddHandler<PerfFanoutHandler, PerfPublishEvent>();
            // Standard uses shared Store discovery; explicit peers are available for diagnostics only.
            return;
        }
        var mesh = options.AddRouteMesh(config.meshName!).Listen(config.meshEndpoint ?? config.listenerEndpoint!);
        foreach (var endpoint in config.peerEndpoints ?? []) mesh.PeerConnections.Connect(endpoint);
        if (config.peerEndpoint is not null) mesh.PeerConnections.Connect(config.peerEndpoint);
        if (config.objectRole == "Server")
        {
            var objects = mesh.Objects().Server();
            if (config.scenario.Contains("actor", StringComparison.Ordinal))
                objects.AddEntrySpot<PerfEntrySpot>().AddActorFactory<PerfActor, PerfActorFactory>("perf-actor", f => f.DisableRelocation());
            else objects.AddSpotFactory<PerfSpot>("perf-spot", f => f.ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide).DisableRelocation());
        }
        else if (config.objectRole == "Client") mesh.Objects().Client();
        if (config.channelName is not null)
        {
            var callerReturns = config.source && config.mode == "send-send" && config.role != "spot";
            var remoteChannel = !config.source && config.role == "channel";
            if (callerReturns) mesh.Channel(config.channelName).Server().AddSendHandler<PerfChannelReplyHandler, PerfEchoReply>();
            else if (remoteChannel) mesh.Channel(config.channelName).Server()
                .AddRequestHandler<PerfChannelRequestHandler, PerfEchoRequest, PerfEchoReply>()
                .AddSendHandler<PerfChannelSendHandler, PerfEchoRequest>();
            else mesh.Channel(config.channelName).Client();
        }
    }
    public async Task PrepareAsync(CancellationToken stopping)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(stopping);
        timeout.CancelAfter(config.workload.setupTimeoutMs);
        try
        {
            sequences = new long[config.workload.logicalStreams ?? 1];
            if (config.objectRole != "None")
            {
                if (config.scenario.Contains("actor", StringComparison.Ordinal))
                {
                    var manager = services.GetRequiredService<IZLinkActorManager>();
                    using var concurrency = new SemaphoreSlim(config.workload.connectConcurrency ?? 256);
                    await Task.WhenAll(config.actorIds.Select(async id =>
                    {
                        await concurrency.WaitAsync(timeout.Token);
                        try
                        {
                            var result = await manager.GetOrCreate(id, "perf-actor").InMesh(config.meshName!)
                                .Timeout(TimeSpan.FromMilliseconds(config.workload.setupTimeoutMs)).Async(timeout.Token);
                            if (result is ZLinkActorCreateResult.Rejected) throw new PerfValidationException("PreparationRejected", "Actor creation rejected.");
                        }
                        finally { concurrency.Release(); }
                    }));
                }
                else
                {
                    var manager = services.GetRequiredService<IZLinkSpotManager>();
                    foreach (var id in config.spotIds) await manager.GetOrCreate(id, "perf-spot").InMesh(config.meshName!)
                        .Timeout(TimeSpan.FromMilliseconds(config.workload.setupTimeoutMs)).Async(timeout.Token);
                }
            }
            var request = measurement.Request(0, checked((ulong)Interlocked.Increment(ref sequences[0])), true);
            if (config.mode == "publish") request = measurement.Request(0, checked((ulong)Interlocked.Increment(ref publishSequence)), true);
            if (config.mode == "publish")
            {
                await PublishAsync(request, timeout.Token);
                measurement.SetupEvidence = [new { kind = "warmupMarkerPublished", source = "IZLinkFanoutClient.Publish.Async", observedValue = request.sequence }];
            }
            else if (config.mode == "send")
            {
                if (config.role == "spot") await DriveAsync(request, timeout.Token);
                else await SendAsync(request, timeout.Token);
                measurement.SetupEvidence = [new { kind = "typedSendProbeAdmission", source = "public typed send terminal", observedValue = request.correlationId }];
            }
            else if (config.mode == "send-send")
            {
                request = WithReturn(request);
                var pending = Register(request, PerfClock.Now);
                var echoTask = pending.Completion!.Task;
                if (config.role == "spot") await DriveAsync(request, timeout.Token);
                else await SendAsync(request, timeout.Token);
                var reply = await echoTask.WaitAsync(TimeSpan.FromMilliseconds(config.workload.correlationExpiryMs), timeout.Token);
                pending.Completion = null;
                ValidateReply(request, reply!);
                measurement.SetupEvidence = [new { kind = "typedSendSendProbeEcho", source = "public send + typed return handler", observedValue = request.correlationId }];
            }
            else
            {
                var reply = config.role == "spot" && config.scenario.StartsWith("s2s-spot", StringComparison.Ordinal)
                    ? (await DriveAsync(request, timeout.Token)).echo! : await RequestAsync(request, timeout.Token);
                ValidateReply(request, reply);
                measurement.SetupEvidence = [new { kind = "typedProbeEcho", source = "public request + full typed payload validation", observedValue = request.correlationId }];
            }
            if (config.objectRole != "None") measurement.SetupEvidence = measurement.SetupEvidence.Prepend((object)new
            { kind = "publicObjectsPrepared", source = "IZLinkActorManager.GetOrCreate / IZLinkSpotManager.GetOrCreate.Async",
                observedValue = new { actorCount = config.actorIds.Length, spotCount = config.spotIds.Length,
                    actorSetupConcurrency = config.workload.connectConcurrency ?? 256 } }).ToArray();
        }
        catch (Exception error) { measurement.RecordDiagnostic(error); }
    }
    public Task RunAsync() => Task.WhenAll(Enumerable.Range(0, config.workload.logicalStreams!.Value)
        .SelectMany(stream => Enumerable.Range(0, config.workload.inflight).Select(_ => LoopAsync(stream))));
    private async Task LoopAsync(int stream)
    {
        // Public admission can complete synchronously (notably Publish.Async); each logical stream must run.
        await Task.Yield();
        while (measurement.CanIssue)
        {
            var sequence = checked((ulong)Interlocked.Increment(ref sequences[stream]));
            if (config.mode == "publish") sequence = checked((ulong)Interlocked.Increment(ref publishSequence));
            var request = measurement.Request(config.mode == "publish" ? 0 : stream, sequence);
            var driven = config.role == "spot" && config.scenario.StartsWith("s2s-spot", StringComparison.Ordinal);
            var started = PerfClock.Now;
            if (driven) measurement.Count("driver.issued");
            else if (!measurement.BeginOperation(out started, request)) break;
            request = request with { sentTicks = DecimalText.Of(started) };
            if (config.mode == "send-send") request = WithReturn(request);
            try
            {
                if (config.mode == "publish") { await PublishAsync(request); measurement.CompleteAdmission(request, started); }
                else if (config.mode == "send")
                {
                    if (driven)
                    {
                        var drive = await DriveAsync(request);
                        if (!drive.started) measurement.Count("driver.notStarted");
                    }
                    else { await SendAsync(request); measurement.CompleteAdmission(request, started); }
                }
                else if (config.mode == "send-send")
                {
                    var pending = Register(request, started);
                    var echoTask = pending.Completion!.Task;
                    try
                    {
                        if (driven)
                        {
                            var drive = await DriveAsync(request);
                            if (!drive.started) { measurement.Count("driver.notStarted"); pending.Closed = 2; pending.Completion = null; continue; }
                        }
                        else { await SendAsync(request); measurement.RecordInitialAdmission(started, config.role == "actorCaller"); }
                    }
                    catch (Exception error) { Finish(pending, null, error); }
                    var remainingNs = pending.Started + config.workload.correlationExpiryMs * 1_000_000L - PerfClock.Now;
                    try { await echoTask.WaitAsync(TimeSpan.FromTicks(Math.Max(1, remainingNs / 100))); }
                    catch (TimeoutException) { Finish(pending, null, new PerfValidationException("CorrelationExpired", "Application echo correlation expired.")); }
                    finally { pending.Completion = null; }
                    if (driven && pending.Closed == 1) measurement.RecordSloSuccess(started);
                }
                else
                {
                    PerfEchoReply? reply;
                    if (driven)
                    {
                        var drive = await DriveAsync(request);
                        if (!drive.started) { measurement.Count("driver.notStarted"); continue; }
                        reply = drive.echo;
                    }
                    else reply = await RequestAsync(request);
                    ValidateReply(request, reply!);
                    if (!driven) measurement.CompleteOperation(started);
                    else measurement.RecordSloSuccess(started);
                }
                if (driven) measurement.RecordAuxiliary("driverLatencyMs", PerfClock.Now - started, PerfClock.Now);
            }
            catch (Exception error)
            {
                if (driven) { measurement.Count("driver.failed"); measurement.RecordDiagnostic(error); }
                else if (measurement.OneWay) measurement.CompleteAdmission(request, started, error);
                else measurement.CompleteOperation(started, error);
            }
            await Task.Yield();
        }
    }
    private PerfEchoRequest WithReturn(PerfEchoRequest request) => request with
    { returnSpotId = config.role == "spot" ? config.spotIds[request.clientId % config.spotIds.Length] : null,
        returnChannel = config.role == "spot" ? null : config.channelName };
    private Correlation Register(PerfEchoRequest request, long started)
    {
        var pending = new Correlation(request, started, measurement.Phase == "setup") { OperationStarted = config.role != "spot" };
        if (!correlations.TryAdd((request.clientId, DecimalText.U64(request.sequence)), pending))
            throw new PerfValidationException("IdentityMismatch", "Correlation reused.");
        return pending;
    }
    public void MarkDrivenStart(PerfEchoRequest request, long remoteStarted)
    {
        if (correlations.TryGetValue((request.clientId, DecimalText.U64(request.sequence)), out var pending)) { pending.Started = remoteStarted; pending.OperationStarted = true; }
    }
    public void Reply(PerfEchoReply reply)
    {
        if (reply.phase == "warmup" && measurement.ResetSeq != "0") { measurement.Count("lateWarmupMessages"); return; }
        if (!correlations.TryGetValue((reply.clientId, DecimalText.U64(reply.sequence)), out var pending))
        { measurement.Count("unknownCorrelation"); return; }
        if (Volatile.Read(ref pending.Closed) != 0)
        { measurement.Count(pending.Closed == 1 ? "duplicateReply" : "lateReply"); return; }
        try
        {
            if (reply.runId != config.runId || reply.cellId != config.cellId || reply.clientId != pending.ClientId ||
                reply.sequence != DecimalText.Of(pending.Sequence) || reply.resetSeq != (pending.Warmup ? "0" : measurement.ResetSeq) ||
                reply.phase != (pending.Warmup ? "warmup" : "measured") ||
                reply.correlationId != $"{config.cellId}/{reply.phase}/{pending.ClientId}/{pending.Sequence}")
                throw new PerfValidationException("IdentityMismatch", "Return echo differs from submitted correlation.");
            measurement.ReplyPattern.Validate(reply.payload); DecimalText.I64(reply.receivedTicks);
            Finish(pending, reply, null);
        }
        catch (Exception error) { Finish(pending, null, error); }
    }
    private void Finish(Correlation pending, PerfEchoReply? reply, Exception? error)
    {
        if (Interlocked.CompareExchange(ref pending.Closed, error is null ? 1 : 2, 0) != 0) return;
        if (!pending.SetupProbe && pending.OperationStarted)
        {
            if (error is PerfValidationException { Kind: "CorrelationExpired" }) measurement.Count("expired");
            measurement.CompleteOperation(pending.Started, error, recordSlo: config.role != "spot");
        }
        pending.Completion?.TrySetResult(reply);
    }
    public void ValidateReply(PerfEchoRequest request, PerfEchoReply reply)
    { PayloadPattern.ValidateIdentity(request, reply); measurement.ReplyPattern.Validate(reply.payload); }
    public ValueTask<PerfEchoReply> RequestAsync(PerfEchoRequest request, CancellationToken token = default)
    {
        if (config.scenario.StartsWith("actor-no-bind", StringComparison.Ordinal))
            return services.GetRequiredService<IZLinkActorClient>().RequestToActor(config.actorIds[request.clientId % config.actorIds.Length], request)
                .Timeout(TimeSpan.FromMilliseconds(config.workload.requestTimeoutMs)).Async<PerfEchoReply>(token);
        return services.GetRequiredService<IZLinkSpotClient>().RequestToSpot(config.spotIds[request.clientId % config.spotIds.Length], request)
            .Timeout(TimeSpan.FromMilliseconds(config.workload.requestTimeoutMs)).Async<PerfEchoReply>(token);
    }
    public ValueTask SendAsync(PerfEchoRequest request, CancellationToken token = default)
    {
        if (config.scenario.StartsWith("actor-no-bind", StringComparison.Ordinal)) return services.GetRequiredService<IZLinkActorClient>()
            .SendToActor(config.actorIds[request.clientId % config.actorIds.Length], request).Async(token);
        return services.GetRequiredService<IZLinkSpotClient>().SendToSpot(config.spotIds[request.clientId % config.spotIds.Length], request).Async(token);
    }
    private ValueTask<PerfDriveReply> DriveAsync(PerfEchoRequest request, CancellationToken token = default) => services.GetRequiredService<IZLinkSpotClient>()
        .RequestToSpot(config.spotIds[request.clientId % config.spotIds.Length], new PerfDriveRequest(request))
        .Timeout(TimeSpan.FromMilliseconds(config.workload.requestTimeoutMs)).Async<PerfDriveReply>(token);
    private ValueTask PublishAsync(PerfEchoRequest request, CancellationToken token = default) => services.GetRequiredService<IZLinkFanoutClient>()
        .Publish(config.channelName!, "perf.echo", new PerfPublishEvent { runId = request.runId, cellId = request.cellId,
            resetSeq = request.resetSeq, phase = request.phase, sequence = request.sequence, topic = "perf.echo",
            sentTicks = request.sentTicks, clockDomainId = request.clockDomainId, payload = request.payload }).Async(token);
}

public sealed class PerfSpot(IZLinkSpotContext context, RoleConfig config) : IZLinkSpot
{
    public IZLinkSpotContext Context { get; } = context;
    public void Configure()
    {
        if (config.source && config.scenario.StartsWith("s2s-spot", StringComparison.Ordinal))
        {
            Context.Handlers.AddPacket<PerfSpotDriveHandler>();
            if (config.mode == "send-send") Context.Handlers.AddPacket<PerfSpotReplyHandler>();
        }
        else if (config.mode is "send" or "send-send") Context.Handlers.AddPacket<PerfSpotSendHandler>();
        else Context.Handlers.AddPacket<PerfSpotRequestHandler>();
    }
}
public sealed class PerfSpotRequestHandler(Measurement measurement) : IZLinkSpotRequestHandler<PerfSpot, PerfEchoRequest, PerfEchoReply>
{
    public async ValueTask<PerfEchoReply> HandleAsync(PerfSpot spot, PerfEchoRequest request, CancellationToken token)
    {
        var received = PerfClock.Now; measurement.HandlerEnter(); measurement.Count("applicationHandlerEntries");
        try
        {
            measurement.ValidateRequest(request);
            if (measurement.Config.mode == "worker-offload")
            {
                var submitted = PerfClock.Now;
                var call = spot.Context.RunCpuWorker(ct => WorkerWork.Run(measurement.Config.workload.workerTaskMillis, ct))
                    .Timeout(TimeSpan.FromMilliseconds(measurement.Config.workload.requestTimeoutMs));
                WorkerObservation result;
                if (measurement.Config.terminal == "yield") { measurement.Count("applicationYieldCalls"); result = await call.Yield(token); }
                else result = await call.Async(token);
                var resumed = PerfClock.Now;
                measurement.RecordAuxiliary("workerCallLatencyMs", resumed - submitted, resumed);
                measurement.RecordAuxiliary("workerTaskLatencyMs", DecimalText.I64(result.endedTicks) - DecimalText.I64(result.startedTicks), resumed);
                measurement.RecordAuxiliary("workerSubmitToStartMs", DecimalText.I64(result.startedTicks) - submitted, resumed);
                measurement.RecordAuxiliary("workerResultToContinuationMs", resumed - DecimalText.I64(result.endedTicks), resumed);
            }
            var reply = PayloadPattern.Reply(request, received); measurement.RecordReply(request); Probe(measurement, request); return reply;
        }
        catch (Exception error) { measurement.RecordDiagnostic(error); throw; }
        finally { measurement.HandlerExit(); }
    }
    internal static void Probe(Measurement measurement, PerfEchoRequest request)
    { if (request.resetSeq == "0") measurement.SetupEvidence = [new { kind = "typedProbeReceipt", source = "public typed application handler", observedValue = request.correlationId }]; }
}
public sealed class PerfSpotDriveHandler(Measurement measurement, PerfScenario scenario) : IZLinkSpotRequestHandler<PerfSpot, PerfDriveRequest, PerfDriveReply>
{
    public async ValueTask<PerfDriveReply> HandleAsync(PerfSpot spot, PerfDriveRequest drive, CancellationToken token)
    {
        var request = drive.echo; var probe = measurement.Phase == "setup";
        measurement.HandlerEnter(); measurement.Count("applicationHandlerEntries");
        try
        {
            measurement.ValidateRequest(request);
            long started = PerfClock.Now;
            if (!probe && !measurement.BeginOperation(out started, request)) return new(false, null);
            request = request with { sentTicks = DecimalText.Of(started) };
            if (measurement.Config.mode == "send-send") scenario.MarkDrivenStart(request, started);
            if (measurement.Config.mode == "request")
            {
                try
                {
                    var call = spot.Context.Outbound.RequestToChannel(measurement.Config.channelName!, request)
                        .Timeout(TimeSpan.FromMilliseconds(measurement.Config.workload.requestTimeoutMs));
                    PerfEchoReply reply;
                    if (measurement.Config.terminal == "yield") { measurement.Count("applicationYieldCalls"); reply = await call.Yield<PerfEchoReply>(token); }
                    else reply = await call.Async<PerfEchoReply>(token);
                    scenario.ValidateReply(request, reply);
                    if (!probe) measurement.CompleteOperation(started, recordSlo: false);
                    PerfSpotRequestHandler.Probe(measurement, request); return new(true, reply);
                }
                catch (Exception error) { if (!probe) measurement.CompleteOperation(started, error); throw; }
            }
            try
            {
                await spot.Context.Outbound.SendToChannel(measurement.Config.channelName!, request).Async(token);
                if (!probe)
                {
                    if (measurement.Config.mode == "send") measurement.CompleteAdmission(request, started);
                    else measurement.RecordInitialAdmission(started, false);
                }
                PerfSpotRequestHandler.Probe(measurement, request); return new(true, null);
            }
            catch (Exception error) { if (!probe && measurement.Config.mode == "send") measurement.CompleteAdmission(request, started, error); throw; }
        }
        catch (Exception error) { measurement.RecordDiagnostic(error); throw; }
        finally { measurement.HandlerExit(); }
    }
}
public sealed class PerfSpotSendHandler(Measurement measurement) : IZLinkSpotPacketHandler<PerfSpot, PerfEchoRequest>
{
    public async ValueTask HandleAsync(PerfSpot spot, PerfEchoRequest request, CancellationToken token)
    {
        var received = PerfClock.Now; measurement.HandlerEnter();
        try
        {
            if (measurement.Config.mode == "send") measurement.RecordReceipt(request, received);
            else
            {
                measurement.ValidateRequest(request);
                var reply = PayloadPattern.Reply(request, received); measurement.RecordReply(request);
                await spot.Context.Outbound.SendToChannel(request.returnChannel!, reply).Async(token);
            }
            PerfSpotRequestHandler.Probe(measurement, request);
        }
        catch (Exception error) { measurement.RecordDiagnostic(error); throw; }
        finally { measurement.HandlerExit(); }
    }
}
public sealed class PerfSpotReplyHandler(PerfScenario scenario) : IZLinkSpotPacketHandler<PerfSpot, PerfEchoReply>
{
    public ValueTask HandleAsync(PerfSpot spot, PerfEchoReply reply, CancellationToken token) { scenario.Reply(reply); return ValueTask.CompletedTask; }
}
public sealed class PerfChannelRequestHandler(Measurement measurement) : IZLinkRequestHandler<PerfEchoRequest, PerfEchoReply>
{
    public ValueTask<PerfEchoReply> HandleAsync(PerfEchoRequest request, IZLinkMessageContext context, CancellationToken token)
    {
        var received = PerfClock.Now; measurement.HandlerEnter();
        try { measurement.ValidateRequest(request); var reply = PayloadPattern.Reply(request, received); measurement.RecordReply(request); PerfSpotRequestHandler.Probe(measurement, request); return ValueTask.FromResult(reply); }
        catch (Exception error) { measurement.RecordDiagnostic(error); throw; }
        finally { measurement.HandlerExit(); }
    }
}
public sealed class PerfChannelSendHandler(Measurement measurement, IZLinkSpotClient spots) : IZLinkSendHandler<PerfEchoRequest>
{
    public async ValueTask HandleAsync(PerfEchoRequest request, IZLinkMessageContext context, CancellationToken token)
    {
        var received = PerfClock.Now; measurement.HandlerEnter();
        try
        {
            if (measurement.Config.mode == "send") measurement.RecordReceipt(request, received);
            else { measurement.ValidateRequest(request); var reply = PayloadPattern.Reply(request, received); measurement.RecordReply(request); await spots.SendToSpot(request.returnSpotId!, reply).Async(token); }
            PerfSpotRequestHandler.Probe(measurement, request);
        }
        catch (Exception error) { measurement.RecordDiagnostic(error); throw; }
        finally { measurement.HandlerExit(); }
    }
}
public sealed class PerfChannelReplyHandler(PerfScenario scenario) : IZLinkSendHandler<PerfEchoReply>
{
    public ValueTask HandleAsync(PerfEchoReply reply, IZLinkMessageContext context, CancellationToken token) { scenario.Reply(reply); return ValueTask.CompletedTask; }
}
public sealed class PerfActor(IZLinkActorContext context) : IZLinkActor { public IZLinkActorContext Context { get; } = context; }
public sealed class PerfActorFactory : IZLinkActorFactory<PerfActor>
{
    public ValueTask<PerfActor> CreateAsync(IZLinkActorContext context, CancellationToken token = default) => ValueTask.FromResult(new PerfActor(context));
}
public sealed class PerfEntrySpot(IZLinkEntrySpotContext context) : IZLinkEntrySpot<PerfActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;
    public void Configure() { Context.Handlers.AddActorPacket<PerfActorRequestHandler, PerfActor>(); Context.Handlers.AddActorPacket<PerfActorSendHandler, PerfActor>(); }
    public ValueTask OnJoinedActorAsync(PerfActor actor, CancellationToken token) => ValueTask.CompletedTask;
    public ValueTask OnLeaveActorAsync(PerfActor actor, CancellationToken token) => ValueTask.CompletedTask;
}
public sealed class PerfActorRequestHandler(Measurement measurement) : IZLinkEntrySpotActorRequestHandler<PerfEntrySpot, PerfActor, PerfEchoRequest, PerfEchoReply>
{
    public ValueTask<PerfEchoReply> HandleAsync(PerfEntrySpot spot, PerfActor actor, IZLinkMessageContext context, PerfEchoRequest request, CancellationToken token)
    {
        var received = PerfClock.Now; measurement.HandlerEnter();
        try { measurement.ValidateRequest(request); var reply = PayloadPattern.Reply(request, received); measurement.RecordReply(request); PerfSpotRequestHandler.Probe(measurement, request); return ValueTask.FromResult(reply); }
        catch (Exception error) { measurement.RecordDiagnostic(error); throw; }
        finally { measurement.HandlerExit(); }
    }
}
public sealed class PerfActorSendHandler(Measurement measurement, IZLinkRouteClient channels) : IZLinkEntrySpotActorSendHandler<PerfEntrySpot, PerfActor, PerfEchoRequest>
{
    public async ValueTask HandleAsync(PerfEntrySpot spot, PerfActor actor, IZLinkMessageContext context, PerfEchoRequest request, CancellationToken token)
    {
        var received = PerfClock.Now; measurement.HandlerEnter();
        try
        {
            if (measurement.Config.mode == "send") measurement.RecordReceipt(request, received);
            else { measurement.ValidateRequest(request); var reply = PayloadPattern.Reply(request, received); measurement.RecordReply(request); await channels.SendToChannel(request.returnChannel!, reply).Async(token); }
            PerfSpotRequestHandler.Probe(measurement, request);
        }
        catch (Exception error) { measurement.RecordDiagnostic(error); throw; }
        finally { measurement.HandlerExit(); }
    }
}
public sealed class PerfFanoutHandler(Measurement measurement) : IZLinkFanoutHandler<PerfPublishEvent>
{
    public ValueTask HandleAsync(PerfPublishEvent message, CancellationToken token)
    {
        var received = PerfClock.Now; measurement.HandlerEnter();
        try
        {
            if (message.topic != "perf.echo") throw new PerfValidationException("IdentityMismatch", "Fanout topic differs.");
            measurement.RecordPublishReceipt(message, received);
            if (message.resetSeq == "0") measurement.SetupEvidence = [new { kind = "warmupMarkerReceived", source = "IZLinkFanoutHandler<PerfPublishEvent>", observedValue = message.sequence }];
            return ValueTask.CompletedTask;
        }
        catch (Exception error) { measurement.RecordDiagnostic(error); throw; }
        finally { measurement.HandlerExit(); }
    }
}
public static class WorkerWork
{
    public static WorkerObservation Run(int millis, CancellationToken token)
    {
        var started = PerfClock.Now; ulong iterations = 0; uint x = 0x12345678;
        do
        {
            for (var i = 0; i < 1024; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; }
            iterations += 1024; token.ThrowIfCancellationRequested();
        } while (PerfClock.Now - started < millis * 1_000_000L);
        return new(DecimalText.Of(started), DecimalText.Of(PerfClock.Now), PerfClock.Domain, DecimalText.Of(iterations), x);
    }
}
