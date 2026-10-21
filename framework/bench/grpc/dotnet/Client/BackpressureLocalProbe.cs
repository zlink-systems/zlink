using System.Diagnostics;
using System.Text.Json;
using Google.Protobuf;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Systems.Zlink;
using WithGrpcBench.Shared;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;

internal static class BackpressureLocalProbe
{
    // Local diagnostic mode: both public transports run alternately in one
    // process. Values include harness/serializer/binding/background allocations.
    internal static async Task RunAsync(string output)
    {
        Directory.CreateDirectory(output);
        using var flow = BenchDiagnostics.FlowTraceEnabled
            ? new TestHostMessageFlowListener(Path.Combine(output, "pattern.flow"))
            : null;
        var builder = Host.CreateApplicationBuilder();
        builder.Logging.ClearProviders();
        builder.Logging.AddConsole();
        builder.Services.AddZLinkFramework(framework =>
        {
            framework.Codecs.Use(ZLinkProtobufCodec.Default);
            if (BenchDiagnostics.FlowTraceEnabled)
                framework.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            framework
                .AddRouteMesh("bench")
                .Listen(0)
                .SetRoutingId(RoutingId.From("bench-server"))
                .AddRouteRequestHandler<Echo, BenchPayload, BenchPayload>("BenchPayload");
        });
        using var serverHost = builder.Build();
        await serverHost.StartAsync();
        var endpoint = serverHost
            .Services.GetRequiredService<IZLinkFrameworkRuntime>()
            .GetListenerStatus(ZLinkListenerKind.RouteMesh, "bench")
            .Endpoint;
        var options = new BenchOptions(
            "zlink-framework-dotnet",
            "request-backpressure",
            1024,
            100,
            8,
            0,
            0,
            30000,
            "",
            "",
            endpoint,
            null,
            "",
            "router",
            output,
            "report.txt",
            "Release",
            TimeSpan.FromSeconds(300)
        );
        await using var framework = await FrameworkBenchTransport.CreateAsync(options);
        using var readyDeadline = new CancellationTokenSource(options.Timeout);
        await framework.WaitForPeerAsync(readyDeadline.Token);
        await framework.ProbeAsync(CancellationToken.None);

        using var rawContext = Systems.Zlink.Zlink.CreateContext();
        using var rawServer = rawContext.CreateRouterSocket();
        rawServer.SetRoutingId(RoutingId.From(BenchRoutingIds.RawRequestServer));
        // Match the existing raw server cleanup polling interval.
        rawServer.Options.ReceiveTimeout = TimeSpan.FromMilliseconds(100);
        rawServer.Bind("tcp://127.0.0.1:0");
        await using var raw = new RawBenchTransport(
            options with
            {
                Implementation = "zlink-dotnet",
                TargetEndpoint = rawServer.Options.LastEndpoint,
            }
        );
        using var stopping = new CancellationTokenSource();
        var receiver = Task.Run(() => ReceiveRaw(rawServer, stopping.Token));
        try
        {
            await raw.ProbeAsync(CancellationToken.None);
            // Warmup and measurement use the same non-windowed admission loop.
            await RunPhase("raw-warmup", raw, output, TimeSpan.FromMilliseconds(200));
            await RunPhase("framework-warmup", framework, output, TimeSpan.FromMilliseconds(200));
            foreach (var cell in new[] { "raw", "framework", "framework", "raw" })
                await RunPhase(
                    cell,
                    cell == "raw" ? raw : framework,
                    output,
                    TimeSpan.FromSeconds(1)
                );
        }
        finally
        {
            stopping.Cancel();
            await receiver;
            await serverHost.StopAsync();
        }
    }

    private static async Task RunPhase(
        string cell,
        IBenchTransport transport,
        string output,
        TimeSpan duration
    )
    {
        var rows = new (long Tick, string Event, long InFlight)[4096];
        var rowCount = 0;
        var droppedSamples = 0;
        var tasks = new HashSet<Task>();
        long issued = 0,
            completed = 0,
            inFlight = 0,
            peak = 0,
            pauses = 0,
            resumes = 0;
        using var process = Process.GetCurrentProcess();
        var cpu = process.TotalProcessorTime;
        var allocated = GC.GetTotalAllocatedBytes(true);
        var begin = Stopwatch.GetTimestamp();
        var end = begin + (long)(duration.TotalSeconds * Stopwatch.Frequency);
        while (Stopwatch.GetTimestamp() < end)
        {
            Task<BenchPayload> reply;
            Task admission;
            issued++;
            var payload = BenchMetricHeaders.CreatePayload(
                1024,
                1,
                BenchPhase.Warmup,
                (ulong)issued
            );
            var live = Interlocked.Increment(ref inFlight);
            peak = Math.Max(peak, live);
            if (transport is FrameworkBenchTransport framework)
            {
                var request = framework.SubmitRequest(payload, CancellationToken.None);
                reply = request.AsTask();
                admission = request.Admitted.AsTask();
            }
            else
            {
                var request = ((RawBenchTransport)transport).SubmitRequest(
                    payload,
                    CancellationToken.None
                );
                reply = request.Reply;
                admission =
                    request.Result == SubmitResult.Backpressured
                        ? request.Admitted
                        : Task.CompletedTask;
            }
            tasks.Add(Observe(reply, payload));
            if (!admission.IsCompleted)
            {
                pauses++;
                Sample("pause");
                if (transport is RawBenchTransport raw)
                    while (!admission.IsCompleted)
                        raw.WaitRequestCompletion(1);
                await admission;
                resumes++;
                Sample("resume");
            }
            else
                await admission;
            if (transport is RawBenchTransport rawTransport)
                rawTransport.WaitRequestCompletion(0);
            if ((issued & 255) == 0)
            {
                Sample("batch");
                tasks.RemoveWhere(static task => task.IsCompletedSuccessfully);
                await Task.Yield();
            }
        }
        var activeElapsed = Stopwatch.GetElapsedTime(begin);
        var completedAtEnd = Interlocked.Read(ref completed);
        var inFlightAtEnd = Interlocked.Read(ref inFlight);
        var cpuAtEnd = process.TotalProcessorTime;
        Sample("active-end");
        if (transport is RawBenchTransport drainRaw)
            while (tasks.Any(static task => !task.IsCompleted))
                drainRaw.WaitRequestCompletion(1);
        await Task.WhenAll(tasks);
        var elapsed = Stopwatch.GetElapsedTime(begin);
        var bytes = GC.GetTotalAllocatedBytes(true) - allocated;
        var cpuMs = (process.TotalProcessorTime - cpu).TotalMilliseconds;
        var report = new
        {
            cell,
            issued,
            completed,
            peak,
            pauses,
            resumes,
            completedAtEnd,
            inFlightAtEnd,
            activeMs = activeElapsed.TotalMilliseconds,
            drainMs = (elapsed - activeElapsed).TotalMilliseconds,
            activeCpuMs = (cpuAtEnd - cpu).TotalMilliseconds,
            rowCount,
            droppedSamples,
            elapsedMs = elapsed.TotalMilliseconds,
            cpuMs,
            allocatedBytes = bytes,
            bytesPerRequest = (double)bytes / issued,
            activeCompletionPerSecond = completedAtEnd / activeElapsed.TotalSeconds,
            cpuUsPerRequest = cpuMs * 1000.0 / completed,
            completionPerSecond = completed / elapsed.TotalSeconds,
        };
        await File.AppendAllTextAsync(
            Path.Combine(output, "pattern.jsonl"),
            JsonSerializer.Serialize(report) + "\n"
        );
        var filename = $"{cell}-{begin}.tsv";
        await File.WriteAllLinesAsync(
            Path.Combine(output, filename),
            rows.Take(rowCount).Select(row => $"{row.Tick - begin}\t{row.Event}\t{row.InFlight}")
        );
        Console.WriteLine(JsonSerializer.Serialize(report));

        void Sample(string kind)
        {
            if (rowCount < rows.Length)
                rows[rowCount++] = (Stopwatch.GetTimestamp(), kind, Interlocked.Read(ref inFlight));
            else
                droppedSamples++;
        }

        async Task Observe(Task<BenchPayload> pending, BenchPayload expected)
        {
            var result = await pending.ConfigureAwait(false);
            if (!result.Body.Equals(expected.Body))
                throw new InvalidOperationException("Echo payload changed.");
            Interlocked.Increment(ref completed);
            Interlocked.Decrement(ref inFlight);
        }
    }

    private static void ReceiveRaw(IRouterSocket socket, CancellationToken stopping)
    {
        using var received = Received.Create();
        while (!stopping.IsCancellationRequested)
        {
            try
            {
                if (!socket.Recv(received))
                    continue;
                var payload = RawWire.Decode(received.Parts[^1].AsReadOnlySpan());
                using var header = Message.From(RawEnvelopeHeaders.Response);
                using var body = Message.Allocate(payload.CalculateSize());
                RawWire.Encode(payload, body.AsSpan());
                received.Reply().Message(header).Message(body).Submit();
            }
            catch (ZlinkRecvException error)
                when (error.Result == ZlinkRecvException.ErrorCode.NoData) { }
        }
    }

    public sealed class Echo : IZLinkRouteRequestHandler<BenchPayload, BenchPayload>
    {
        public ValueTask<BenchPayload> HandleAsync(
            BenchPayload request,
            ZLinkRouteMessageContext context,
            CancellationToken cancellationToken
        ) => ValueTask.FromResult(request);
    }
}
