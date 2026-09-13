using Systems.Zlink.Stream.Connector.Contracts;

namespace ZLink.Framework.Perf;

// §11.1: independent CS process -> Session process; physical connector per global client ID.
// Request.Async<PerfEchoReply> through full identity/byte validation is one operation.
// request64/response4096 typed JSON, ordinary, Immediate dispatch. CS Actor cells bind during setup.
// Server logical stream, Actor, Spot, worker and fanout metrics are not applicable.
public sealed class SessionEchoOnlyScenario(EndpointManifest manifest, Measurement measurement, int index) : IAsyncDisposable
{
    private readonly List<(int id, IZlinkStreamConnector connector)> connectors = [];
    private readonly List<IZlinkStreamConnector> owned = [];
    private long[] sequences = [];
    public async Task PrepareAsync()
    {
        var workload = manifest.workload;
        var total = workload.connections!.Value;
        var quotient = total / workload.clientCount;
        var remainder = total % workload.clientCount;
        var count = quotient + (index < remainder ? 1 : 0);
        var first = index * quotient + Math.Min(index, remainder);
        sequences = new long[count];
        var endpoint = new Uri(manifest.roles.Single(r => r.streamEndpoint is not null).streamEndpoint!);
        var evidence = new object[count];
        measurement.SetupEvidence = evidence;
        using var timeout = new CancellationTokenSource(workload.setupTimeoutMs);
        using var concurrency = new SemaphoreSlim(workload.connectConcurrency!.Value);
        try
        {
            await Task.WhenAll(Enumerable.Range(0, count).Select(async local =>
            {
                await concurrency.WaitAsync(timeout.Token);
                var started = PerfClock.Now;
                var id = first + local;
                var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
                {
                    Endpoint = endpoint, DispatchMode = ZlinkStreamDispatchMode.Immediate,
                    ConnectTimeout = TimeSpan.FromMilliseconds(workload.setupTimeoutMs),
                    RequestTimeout = TimeSpan.FromMilliseconds(workload.requestTimeoutMs)
                });
                lock (owned) owned.Add(connector);
                try
                {
                    await connector.Connect.Async(timeout.Token);
                    if (manifest.scenario != "session-echo-only")
                    {
                        var binding = await connector.Request(new PerfBindRequest(manifest.runId, manifest.cellId, id))
                            .Timeout(TimeSpan.FromMilliseconds(workload.setupTimeoutMs)).Async<PerfBindReply>(timeout.Token);
                        if (!binding.bound) throw new PerfValidationException("PreparationRejected", "Session actor bind rejected.");
                    }
                    var request = measurement.Request(id, (ulong)Interlocked.Increment(ref sequences[local]), probe: true);
                    var reply = await connector.Request(request).Timeout(TimeSpan.FromMilliseconds(workload.requestTimeoutMs))
                        .Async<PerfEchoReply>(timeout.Token);
                    PayloadPattern.ValidateIdentity(request, reply);
                    measurement.ReplyPattern.Validate(reply.payload);
                    if (!connector.IsConnected) throw new InvalidOperationException("Connector lost its connection during setup.");
                    lock (connectors)
                    {
                        connectors.Add((id, connector));
                        measurement.Connected++;
                    }
                    evidence[local] = new { kind = "connectorSetupAndTypedProbe", source = "Connect.Async + IsConnected + Request.Async<PerfEchoReply>",
                        observedValue = new { clientId = id, state = connector.State.ToString(), connector.IsConnected,
                            setupLatencyNs = DecimalText.Of(PerfClock.Now - started), request.correlationId } };
                }
                catch (Exception error)
                {
                    lock (connectors) measurement.ConnectionFailures++;
                    measurement.RecordDiagnostic(error);
                    evidence[local] = new { kind = "connectorSetupFailure", source = error.GetType().FullName,
                        observedValue = new { clientId = id, error.Message, setupLatencyNs = DecimalText.Of(PerfClock.Now - started) } };
                }
                finally { concurrency.Release(); }
            }));
        }
        catch (OperationCanceledException error)
        {
            measurement.RecordDiagnostic(error);
        }
    }
    public Task RunAsync()
    {
        var first = manifest.workload.connections!.Value / manifest.workload.clientCount * index +
            Math.Min(index, manifest.workload.connections.Value % manifest.workload.clientCount);
        return Task.WhenAll(connectors.SelectMany(entry => Enumerable.Range(0, manifest.workload.inflight)
            .Select(_ => LoopAsync(entry.id, entry.id - first, entry.connector))));
    }
    private async Task LoopAsync(int id, int local, IZlinkStreamConnector connector)
    {
        while (measurement.CanIssue)
        {
            var request = measurement.Request(id, checked((ulong)Interlocked.Increment(ref sequences[local])));
            if (!measurement.BeginOperation(out var started)) break;
            request = request with { sentTicks = DecimalText.Of(started) };
            try
            {
                var reply = await connector.Request(request).Timeout(TimeSpan.FromMilliseconds(manifest.workload.requestTimeoutMs))
                    .Async<PerfEchoReply>();
                PayloadPattern.ValidateIdentity(request, reply);
                measurement.ReplyPattern.Validate(reply.payload);
                measurement.CompleteOperation(started);
            }
            catch (Exception error) { measurement.CompleteOperation(started, error); }
        }
    }
    public async ValueTask DisposeAsync()
    {
        foreach (var connector in owned) await connector.DisposeAsync();
    }
}
