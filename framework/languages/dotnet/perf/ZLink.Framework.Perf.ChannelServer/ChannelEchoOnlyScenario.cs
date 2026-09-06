using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Handlers;

namespace ZLink.Framework.Perf;

// §11.2: two Channel processes, manual RouteMesh or ClientServer, no Store/objects.
// Source public request -> typed identity/full-byte validation is one operation.
// JSON payloads: 1024/4096, request/ordinary. Connector/Actor/Spot/worker/fanout metrics do not apply.
public sealed class ChannelEchoOnlyScenario(IZLinkRouteClient client, Measurement measurement,
    IZLinkRouteMeshRuntime meshRuntime, IZLinkClientServerRuntime channelRuntime)
{
    private readonly RoleConfig config = measurement.Config;
    private long[] sequences = [];
    public async Task PrepareAsync(CancellationToken stopping)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(stopping);
        timeout.CancelAfter(config.workload.setupTimeoutMs);
        try
        {
            // ObserveAsync is a change stream, not an initial snapshot (monitoring §6).
            // Query public status until setup evidence is ready; never retry the probe call.
            while (true)
            {
                timeout.Token.ThrowIfCancellationRequested();
                if (config.topology == "routemesh")
                {
                    var status = meshRuntime.GetStatus(config.meshName!);
                    if (status.IsReady && status.Channels.Any(c => c.ChannelName == config.channelName && c.IsReady && c.ReadyTargetCount > 0)) break;
                }
                else
                {
                    var status = channelRuntime.GetStatus(config.channelName!);
                    if (status.IsReady && status.ReadyTargetCount > 0) break;
                }
                await Task.Yield();
            }
            sequences = new long[config.workload.logicalStreams!.Value];
            var request = measurement.Request(0, (ulong)Interlocked.Increment(ref sequences[0]), probe: true);
            var reply = await client.RequestToChannel(config.channelName!, request)
                .Timeout(TimeSpan.FromMilliseconds(config.workload.requestTimeoutMs)).Async<PerfEchoReply>(timeout.Token);
            PayloadPattern.ValidateIdentity(request, reply);
            measurement.Pattern.Validate(reply.payload);
            measurement.SetupEvidence = [new { kind = "typedProbeEcho", source = "IZLinkRouteClient.RequestToChannel.Async<PerfEchoReply>",
                observedValue = new { request.correlationId, reply.receivedTicks, reply.clockDomainId } }];
        }
        catch (Exception error) { measurement.RecordDiagnostic(error); }
    }
    public Task RunAsync() => Task.WhenAll(Enumerable.Range(0, config.workload.logicalStreams!.Value)
        .SelectMany(stream => Enumerable.Range(0, config.workload.inflight).Select(_ => LoopAsync(stream))));
    private async Task LoopAsync(int stream)
    {
        while (measurement.CanIssue)
        {
            var request = measurement.Request(stream, checked((ulong)Interlocked.Increment(ref sequences[stream])));
            if (!measurement.BeginOperation(out var started)) break;
            request = request with { sentTicks = DecimalText.Of(started) };
            try
            {
                var reply = await client.RequestToChannel(config.channelName!, request)
                    .Timeout(TimeSpan.FromMilliseconds(config.workload.requestTimeoutMs)).Async<PerfEchoReply>();
                PayloadPattern.ValidateIdentity(request, reply);
                measurement.Pattern.Validate(reply.payload);
                measurement.CompleteOperation(started);
            }
            catch (Exception error) { measurement.CompleteOperation(started, error); }
        }
    }
}
public sealed class ChannelEchoHandler(Measurement measurement) : IZLinkRequestHandler<PerfEchoRequest, PerfEchoReply>
{
    public ValueTask<PerfEchoReply> HandleAsync(PerfEchoRequest request, IZLinkMessageContext context, CancellationToken cancellationToken)
    {
        var received = PerfClock.Now;
        measurement.HandlerEnter();
        try
        {
            measurement.ValidateRequest(request);
            var reply = PayloadPattern.Reply(request, received);
            measurement.RecordReply(request);
            if (measurement.Phase == "setup") measurement.SetupEvidence =
                [new { kind = "typedProbeReply", source = "IZLinkRequestHandler<PerfEchoRequest,PerfEchoReply>", observedValue = request.correlationId }];
            return ValueTask.FromResult(reply);
        }
        catch (Exception error) { measurement.RecordDiagnostic(error); throw; }
        finally { measurement.HandlerExit(); }
    }
}
