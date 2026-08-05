using AutomaticTurnDispatch.Server.Play.Spots;
using AutomaticTurnDispatch.Shared;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;
using Zlink.HttpClient;

namespace AutomaticTurnDispatch.Server.Play.Handlers;

[ZLinkSpotPacketHandler("CounterResetMsg")]
internal sealed class CounterResetHandler(EvidenceStore evidence)
    : IZLinkSpotPacketHandler<AwaitProbeSpot, CounterResetMsg>
{
    public ValueTask HandleAsync(
        AwaitProbeSpot spot,
        CounterResetMsg request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        spot.ResetCounter();
        evidence.Add($"counter-reset|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}");
        return ValueTask.CompletedTask;
    }
}

[ZLinkSpotPacketHandler("CounterAwaitMsg")]
internal sealed class CounterAwaitHandler(
    EvidenceStore evidence,
    IZLinkRouteClient routeClient)
    : IZLinkSpotPacketHandler<AwaitProbeSpot, CounterAwaitMsg>
{
    public async ValueTask HandleAsync(
        AwaitProbeSpot spot,
        CounterAwaitMsg request,
        CancellationToken cancellationToken)
    {
        var observed = spot.ReadCounter();
        evidence.Add(
            $"counter-{request.Terminator}-started|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|request={request.RequestId}|operation={request.OperationId}|observed={observed}");
        var call = routeClient.RequestToChannel(
                AutomaticTurnDispatchNames.DelayChannel,
                new DelayReq(request.RequestId, request.DelayMs, request.OperationId))
            .Timeout(TimeSpan.FromSeconds(5));
        await TurnTerminator.Complete<DelayRes>(call, request.Terminator, cancellationToken);
        spot.WriteCounter(observed + 1);
        evidence.Add(
            $"counter-{request.Terminator}-completed|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|request={request.RequestId}|operation={request.OperationId}|value={spot.ReadCounter()}");
    }
}

[ZLinkSpotRequestHandler("CounterReadReq")]
internal sealed class CounterReadHandler
    : IZLinkSpotRequestHandler<AwaitProbeSpot, CounterReadReq, CounterReadRes>
{
    public ValueTask<CounterReadRes> HandleAsync(
        AwaitProbeSpot spot,
        CounterReadReq request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(new CounterReadRes(request.RequestId, spot.ReadCounter()));
    }
}

[ZLinkSpotPacketHandler("HttpAwaitMsg")]
internal sealed class HttpAwaitHandler(
    [FromKeyedServices("external-api")] ZLinkHttpServerClient client,
    EvidenceStore evidence)
    : IZLinkSpotPacketHandler<AwaitProbeSpot, HttpAwaitMsg>
{
    public async ValueTask HandleAsync(
        AwaitProbeSpot spot,
        HttpAwaitMsg request,
        CancellationToken cancellationToken)
    {
        evidence.Add(
            $"http-{request.Terminator}-started|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}");
        evidence.Add(
            $"http-{request.Terminator}-{(request.Terminator == "yield" ? "released" : "held")}|rid={evidence.Rid}"
            + $"|spot={spot.Context.SpotId}|request={request.RequestId}");
        var response = request.Terminator == "yield"
            ? await spot.Context.RunIoWorker(async ct =>
                await client.Get("/delay")
                    .Query("requestId", request.RequestId)
                    .Query("marker", request.Terminator)
                    .Query("delayMs", request.DelayMs.ToString())
                    .Async<ExternalDelayRes>(ct))
                .Yield(cancellationToken)
            : await client.Get("/delay")
                .Query("requestId", request.RequestId)
                .Query("marker", request.Terminator)
                .Query("delayMs", request.DelayMs.ToString())
                .Async<ExternalDelayRes>(cancellationToken);
        evidence.Add(
            $"http-{request.Terminator}-resumed|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|request={request.RequestId}|marker={response.Body.Marker}");
        evidence.Add(
            $"http-{request.Terminator}-completed|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}");
    }
}

[ZLinkSpotPacketHandler("IoWorkerAwaitMsg")]
internal sealed class IoWorkerAwaitHandler(
    [FromKeyedServices("external-api")] ZLinkHttpServerClient client,
    EvidenceStore evidence)
    : IZLinkSpotPacketHandler<AwaitProbeSpot, IoWorkerAwaitMsg>
{
    public async ValueTask HandleAsync(
        AwaitProbeSpot spot,
        IoWorkerAwaitMsg request,
        CancellationToken cancellationToken)
    {
        evidence.Add(
            $"io-worker-started|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}"
            + $"|operation={request.OperationId}|thread={Environment.CurrentManagedThreadId}");
        var call = spot.Context.RunIoWorker(async ct =>
        {
            var response = await client.Get("/delay")
                .Query("requestId", request.RequestId)
                .Query("marker", request.OperationId)
                .Query("delayMs", request.DelayMs.ToString())
                .Async<ExternalDelayRes>(ct);
            return response.Body;
        });
        evidence.Add(
            $"io-worker-released|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}"
            + $"|operation={request.OperationId}");
        var result = await call.Yield(cancellationToken);
        evidence.Add(
            $"io-worker-completed|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}"
            + $"|operation={request.OperationId}|marker={result.Marker}");
    }
}

[ZLinkSpotPacketHandler("CpuWorkerAwaitMsg")]
internal sealed class CpuWorkerAwaitHandler(EvidenceStore evidence)
    : IZLinkSpotPacketHandler<AwaitProbeSpot, CpuWorkerAwaitMsg>
{
    public async ValueTask HandleAsync(
        AwaitProbeSpot spot,
        CpuWorkerAwaitMsg request,
        CancellationToken cancellationToken)
    {
        var callerThread = Environment.CurrentManagedThreadId;
        evidence.Add(
            $"cpu-worker-{request.Terminator}-started|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|request={request.RequestId}|caller-thread={callerThread}");
        var call = spot.Context.RunCpuWorker(ct =>
        {
            ct.ThrowIfCancellationRequested();
            Thread.Sleep(request.DelayMs);
            return Environment.CurrentManagedThreadId;
        });
        evidence.Add(
            $"cpu-worker-{request.Terminator}-{(request.Terminator == "yield" ? "released" : "held")}|rid={evidence.Rid}"
            + $"|spot={spot.Context.SpotId}|request={request.RequestId}");
        var workerThread = request.Terminator == "yield"
            ? await call.Yield(cancellationToken)
            : await call.Async(cancellationToken);
        evidence.Add(
            $"cpu-worker-{request.Terminator}-completed|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|request={request.RequestId}|caller-thread={callerThread}|worker-thread={workerThread}");
    }
}
