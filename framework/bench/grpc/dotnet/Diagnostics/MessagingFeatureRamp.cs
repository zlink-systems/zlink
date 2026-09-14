// Diagnostic only: compiled into the existing Framework test friend assembly.
// No product API or runtime policy is changed by this executable.
using System.Diagnostics;
using System.Net.Http.Json;
using System.Text;
using System.Text.Json;
using Google.Protobuf;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Systems.Zlink;
using Systems.Zlink.Framework.Runtime.Protocol;
using WithGrpcBench.Shared;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Codecs;
using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Diagnostics;

internal static class MessagingFeatureRamp
{
    private static readonly RoutingId TargetRid = RoutingId.From("bench-server");
    private static readonly ZLinkCodecRegistryBuilder Codecs = CreateCodecs();
    private static readonly byte[] RawRequest = RawHeader(1);
    private static readonly byte[] RawResponse = RawHeader(2);
    private static readonly TimeSpan DefaultRequestTimeout =
        new Zlink.Framework.Runtime.Configuration.ZLinkFrameworkRegistration().DefaultRequestTimeout;

#if ZLINK_FIXED_CODEC_BENCH
    // Fixed builds remove runtime feature selection. Envelope adds only the
    // real header processing to the existing codec/stripped transport stage.
#if ZLINK_FIXED_WIRE_BENCH
    private const string BenchStage = "wire";
#elif ZLINK_FIXED_ENVELOPE_BENCH
    private const string BenchStage = "envelope";
#else
    private const string BenchStage = "core";
#endif
    private const bool BenchWithoutDeadline = false;
    private const bool BenchMinimalHeaderRead = false;
    private const bool BenchServerMinimalHeaderRead = false;
    private const bool BenchServerReplyHeaderReference = false;
    private const bool BenchDirectBodyOwner = true;
#if ZLINK_FIXED_PERMIT_BENCH && !ZLINK_FIXED_NO_HOST_PERMIT_BENCH
    private const int BenchPermitBatchSize = 1;
#else
    private const int BenchPermitBatchSize = 0;
#endif
    private const int BenchIngressYieldInterval = 0;
#if ZLINK_FIXED_MAILBOX_BENCH
    private const bool BenchInlineMailbox = true;
#else
    private const bool BenchInlineMailbox = false;
#endif
#if ZLINK_FIXED_RECEIVE_OWNER_BENCH
    private const bool BenchFreshReceived = true;
#else
    private const bool BenchFreshReceived = false;
#endif
#if ZLINK_FIXED_WORKER_BENCH
    private const int BenchMailboxWorkers = 1;
#else
    private const int BenchMailboxWorkers = 0;
#endif
    private const bool BenchReadinessWait = false;
    private const bool BenchWorkerTiming = false;
#if ZLINK_FIXED_DEDICATED_INGRESS_BENCH
    private const bool BenchDedicatedIngress = true;
#else
    private const bool BenchDedicatedIngress = false;
#endif
#if ZLINK_FIXED_CHANNEL_HANDOFF_BENCH
    private const bool BenchChannelHandoff = true;
#else
    private const bool BenchChannelHandoff = false;
#endif
#if ZLINK_FIXED_NO_HOST_PERMIT_BENCH
    private const bool BenchNoPermitExplicit = true;
#else
    private const bool BenchNoPermitExplicit = false;
#endif
#if ZLINK_FIXED_MAILBOX_BENCH
    private const string FixedCodecName = "mailbox";
#elif ZLINK_FIXED_WIRE_BENCH
    private const string FixedCodecName = "wire";
#elif ZLINK_FIXED_ENVELOPE_BENCH
    private const string FixedCodecName = "envelope";
#elif ZLINK_FIXED_FRAMEWORK_CODEC
    private const string FixedCodecName = "codec";
#else
    private const string FixedCodecName = "core";
#endif
#else
    private static readonly string BenchStage = Environment.GetEnvironmentVariable("BENCH_DIAGNOSTIC_STAGE") ?? "core";
    private static readonly bool BenchWithoutDeadline =
        Environment.GetEnvironmentVariable("BENCH_DIAGNOSTIC_NO_DEADLINE") == "1";
    // Functional ablation only: keep the actual envelope bytes and writers,
    // but do not interpret/validate fields unused by this fixed echo fixture.
    private static readonly bool BenchMinimalHeaderRead =
        Environment.GetEnvironmentVariable("BENCH_DIAGNOSTIC_MINIMAL_HEADER_READ") == "1";
    // Server-only ablation: unlike the older flag, client reply decoding and
    // validation remain on their complete production path.
    private static readonly bool BenchServerMinimalHeaderRead =
        Environment.GetEnvironmentVariable("BENCH_DIAGNOSTIC_SERVER_MINIMAL_HEADER_READ") == "1";
    // Diagnostic lower bound only. Constant response fields come from the
    // real codec; the varying correlation token is copied from its request.
    private static readonly bool BenchServerReplyHeaderReference =
        Environment.GetEnvironmentVariable("BENCH_DIAGNOSTIC_SERVER_REPLY_HEADER_REFERENCE") == "1";
    // Ablation only: match the codec's body owner without changing Core or
    // the normal benchmark. Final multipart ownership remains a wire feature.
    private static readonly bool BenchDirectBodyOwner =
        Environment.GetEnvironmentVariable("BENCH_DIAGNOSTIC_DIRECT_BODY_OWNER") == "1";
    // One isolated feature on top of wire/header: the real host-shared permit
    // lifecycle, without a mesh node, mailbox, worker handoff, or handler DI.
    private static readonly int BenchPermitBatchSize = int.Parse(
        Environment.GetEnvironmentVariable("BENCH_DIAGNOSTIC_PERMIT_BATCH_SIZE") ?? "0");
    // Isolate scheduling alone, keeping wire, payloads, receiver ownership and
    // inline handler unchanged. This does not enable mesh/mailbox/DI features.
    private static readonly int BenchIngressYieldInterval = int.Parse(
        Environment.GetEnvironmentVariable("BENCH_DIAGNOSTIC_INGRESS_YIELD_INTERVAL") ?? "0");
    private static readonly bool BenchInlineMailbox =
        Environment.GetEnvironmentVariable("BENCH_DIAGNOSTIC_INLINE_MAILBOX") == "1";
    private static readonly bool BenchFreshReceived =
        Environment.GetEnvironmentVariable("BENCH_DIAGNOSTIC_FRESH_RECEIVED") == "1";
    private static readonly int BenchMailboxWorkers = int.Parse(
        Environment.GetEnvironmentVariable("BENCH_DIAGNOSTIC_MAILBOX_WORKERS") ?? "0");
    private static readonly bool BenchReadinessWait =
        Environment.GetEnvironmentVariable("BENCH_DIAGNOSTIC_READINESS_WAIT") == "1";
    private static readonly bool BenchWorkerTiming =
        Environment.GetEnvironmentVariable("BENCH_DIAGNOSTIC_WORKER_TIMING") == "1";
#endif
    private static long WorkerReceiveTicks, WorkerEnqueueTicks, WorkerClaimTicks, WorkerReplyTicks;
    private static long WorkerReceiveCount, WorkerEnqueueCount, WorkerClaimCount, WorkerReplyCount;
    private static long WorkerWaitTicks, WorkerWaitCount, WorkerPendingBytes;
    private static long WorkerCommandWaitTicks, WorkerCommandWaitCount;
    private static int DiagnosticScalarSink;

    public static async Task<int> Run(string[] args)
    {
#if ZLINK_FIXED_DEDICATED_INGRESS_BENCH
        if (args.Contains("--worker-lifecycle-fidelity"))
        {
            await VerifyWorkerLifecycle();
            return 0;
        }
#endif
        if (args.Contains("--fixed-codec-info") || args.Contains("--fixed-codec-fidelity"))
        {
#if ZLINK_FIXED_CODEC_BENCH
            if (args.Contains("--fixed-codec-fidelity"))
                VerifyFixedCodecMessages();
            else
                Console.WriteLine(JsonSerializer.Serialize(new { diagnosticOnly = true,
                    fixedCodec = FixedCodecName, featureSelection = "compile-time",
                    directBodyOwner = BenchDirectBodyOwner, envelopeHeader = BenchStage is "envelope" or "wire",
                    applicationWire = BenchStage == "wire",
                    mailbox = BenchInlineMailbox && !BenchChannelHandoff, workers = BenchMailboxWorkers,
                    permitBatchSize = BenchPermitBatchSize, freshReceived = BenchFreshReceived,
                    ingressYieldInterval = BenchIngressYieldInterval,
                    dedicatedIngress = BenchDedicatedIngress,
                    noPermitExplicit = BenchNoPermitExplicit,
                    capacityComparison = BenchNoPermitExplicit ? "host-permit-removed-no-replacement-bound" : "host-permit-preserved",
                    handoffPrimitive = BenchChannelHandoff ? "bcl-channel-control" : "mesh-mailbox" }));
            return 0;
#else
            throw new ArgumentException("This executable was not built with MessagingFixedCodec.");
#endif
        }
        if (args.Contains("--idle-readiness"))
        {
            MeasureIdleReadiness();
            return 0;
        }
        if (args.Contains("--reply-header-fidelity"))
        {
            var template = CreateReplyHeaderReferenceTemplate();
            foreach (var correlation in new[] { "a", new string('f', 128), "quote\" slash\\ unicode-한글-😀" })
            {
                var request = ZLinkClientCallCodec.CreateEnvelope(ZLinkMessageKind.Request,
                    "bench", "BenchPayload", DefaultRequestTimeout) with { CorrelationId = correlation };
                using var input = ZLinkEnvelopeCodec.EncodeHeader(request, "application/x-protobuf");
                using var expected = ZLinkEnvelopeCodec.EncodeHeader(
                    ZLinkChannelReplyWriter.CreateReplyHeader(ZLinkMessageKind.Response, "bench", request), "application/x-protobuf");
                using var actual = EncodeReplyHeaderReference(template, input.AsReadOnlySpan());
                if (!actual.AsReadOnlySpan().SequenceEqual(expected.AsReadOnlySpan())
                    || ZLinkEnvelopeCodec.DecodeHeader(actual) != ZLinkEnvelopeCodec.DecodeHeader(expected))
                    throw new InvalidOperationException("Reply reference changes envelope bytes or decoded values.");
            }
            Console.WriteLine("Reply header reference fidelity passed, including escaped and variable-length correlations.");
            return 0;
        }
        if (args.Contains("--header-format-size"))
        {
            var header = ZLinkClientCallCodec.CreateEnvelope(ZLinkMessageKind.Request,
                "bench", "BenchPayload", DefaultRequestTimeout);
            using var encoded = ZLinkEnvelopeCodec.EncodeHeader(header, "application/x-protobuf");
            using var json = JsonDocument.Parse(encoded.ToArray());
            var fields = new Dictionary<string, object?>();
            foreach (var field in json.RootElement.EnumerateObject())
                fields.Add(field.Name, field.Value.ValueKind switch
                {
                    JsonValueKind.String => field.Value.GetString(),
                    JsonValueKind.Number => field.Value.GetInt32(),
                    JsonValueKind.Null => null,
                    _ => throw new InvalidOperationException("Unexpected size fixture field.")
                });
            var namedMap = MessagePack.MessagePackSerializer.Serialize(fields);
            var decoded = MessagePack.MessagePackSerializer.Deserialize<Dictionary<string, object?>>(namedMap);
            if (decoded.Count != fields.Count || fields.Any(field =>
                !decoded.TryGetValue(field.Key, out var value)
                || (value is null) != (field.Value is null)
                || Convert.ToString(value, System.Globalization.CultureInfo.InvariantCulture)
                    != Convert.ToString(field.Value, System.Globalization.CultureInfo.InvariantCulture)))
                throw new InvalidOperationException("MessagePack size fixture changed scalar values.");
            // Size-only alternatives, not proposed field IDs or production codecs.
            var numberedMap = fields.Values.Select((value, index) => (value, index))
                .ToDictionary(entry => entry.index, entry => entry.value);
            using var body = ZLinkEnvelopeCodec.EncodeBody(
                BenchMetricHeaders.CreateRequestPayload(1, BenchPhase.Active, 42), typeof(BenchPayload), Codecs);
            using var packed = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage([encoded, body]);
            using var service = ZLinkServiceWireCodec.EncodeApplicationMessage(
                ServiceWireConstants.Command.NodeRequest, 1, null, false);
            Console.WriteLine(JsonSerializer.Serialize(new { diagnosticOnly = true, sizeOnly = true,
                deadlineRepresentation = "same string in all formats", jsonBytes = encoded.Size,
                applicationRequestBytes = BenchMetricHeaders.RequestPayloadSize,
                encodedBodyBytes = body.Size, multipartFramingBytes = packed.Size - encoded.Size - body.Size,
                serviceHeaderBytes = service.Size, transportPayloadBytes = packed.Size + service.Size,
                messagePackNamedMapBytes = namedMap.Length,
                messagePackNumberedMapBytes = MessagePack.MessagePackSerializer.Serialize(numberedMap).Length,
                messagePackArrayBytes = MessagePack.MessagePackSerializer.Serialize(fields.Values.ToArray()).Length }));
            return 0;
        }
        if (args.Contains("--payload-fidelity"))
        {
            foreach (var size in new[] { 64, 4096 })
            {
                var payload = BenchMetricHeaders.CreatePayload(size, 1, BenchPhase.Active, 42);
                using var encoded = EncodeBenchPayload(payload);
                if (!encoded.AsReadOnlySpan().SequenceEqual(payload.ToByteArray())
                    || !DecodeBenchPayload(encoded).Equals(payload))
                    throw new InvalidOperationException("Body owner ablation changed payload bytes or values.");
            }
            Console.WriteLine($"Payload fidelity passed: stage={BenchStage};directBodyOwner={BenchDirectBodyOwner}");
            return 0;
        }
        if (args.Contains("--mailbox-cost"))
        {
            await MeasureMailboxCost(Value(args, "--output", "mailbox-cost.json"),
                int.Parse(Value(args, "--iterations", "200000")),
                int.Parse(Value(args, "--records-per-turn", "1")));
            return 0;
        }
        if (args.Contains("--state-lane-cost"))
        {
            await MeasureStateLaneCost(Value(args, "--output", "state-lane-cost.json"),
                int.Parse(Value(args, "--iterations", "1000000")));
            return 0;
        }
        if (args.Contains("--wire-cost"))
        {
            await MeasureWireCost(Value(args, "--output", "wire-cost.json"),
                int.Parse(Value(args, "--iterations", "2000000")));
            return 0;
        }
        if (args.Contains("--envelope-cost"))
        {
            await MeasureEnvelopeCost(Value(args, "--output", "envelope-cost.json"),
                int.Parse(Value(args, "--iterations", "100000")));
            return 0;
        }
        if (args.Contains("--pending-cost"))
        {
            await MeasurePendingCost(Value(args, "--output", "pending-cost.json"));
            return 0;
        }
        if (args.Contains("--permit-cost"))
        {
            await MeasurePermitCost(Value(args, "--output", "permit-cost.json"),
                int.Parse(Value(args, "--iterations", "200000")));
            return 0;
        }
        if (args.Contains("--codec-cost"))
        {
            await MeasureCodecCost(Value(args, "--output", "codec-cost.json"));
            return 0;
        }
        var stage = Value(args, "--stage", "core");
        if (stage is not ("core" or "codec" or "envelope" or "wire" or "managed" or "permits" or "full"))
            throw new ArgumentException("Unknown feature-ramp stage.");
        var endpoint = Value(args, "--endpoint", "tcp://127.0.0.1:5234");
        var statsUrl = Value(args, "--stats-url", "http://127.0.0.1:5235");
        ThreadPool.GetMinThreads(out var workers, out var ports);
        if (Value(args, "--role", "source") == "target")
        {
            Console.WriteLine($"RAMP_CONFIG stage={stage} targetServerGc={System.Runtime.GCSettings.IsServerGC}");
            if (args.Contains("--bench-baseline"))
            {
                if (BenchStage is not ("core" or "codec" or "codec-encode" or "codec-decode" or "envelope"
                    or "envelope-outbound" or "envelope-target" or "wire" or "wire-codec"))
                    throw new ArgumentException("Unknown baseline feature.");
                await RunBenchBaseline(endpoint,
                    Value(args, "--command-endpoint", "tcp://127.0.0.1:5208"),
                    Value(args, "--metrics-url", statsUrl));
                return 0;
            }
            await RunTarget(stage, endpoint, statsUrl);
            return 0;
        }
        ThreadPool.SetMinThreads(Math.Max(workers, 8 + Environment.ProcessorCount * 2), ports);
        var warmup = int.Parse(Value(args, "--warmup", "3"));
        var duration = int.Parse(Value(args, "--duration", "10"));
        await using var source = await Source.Create(stage, endpoint);
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(60 + warmup + duration));
        // Admission is part of the managed/full stages, not a benchmark retry.
        await source.WaitReady(deadline.Token);
        var warmUntil = Stopwatch.GetTimestamp() + warmup * Stopwatch.Frequency;
        ulong sequence = 0;
        do
        {
            var request = BenchMetricHeaders.CreateRequestPayload(1, BenchPhase.Warmup, sequence++);
            Validate(await source.Request(request, deadline.Token), request);
        } while (Stopwatch.GetTimestamp() < warmUntil);
        long completed = 0;
        double latencySumUs = 0;
        var samples = new List<double>(200000);
        var started = Stopwatch.GetTimestamp();
        var until = started + duration * Stopwatch.Frequency;
        while (Stopwatch.GetTimestamp() < until)
        {
            var request = BenchMetricHeaders.CreateRequestPayload(1, BenchPhase.Active, (ulong)completed);
            var reply = await source.Request(request, deadline.Token);
            Validate(reply, request);
            BenchMetricHeaders.TryDecode(reply, out var header);
            var latencyUs = Math.Max(0, BenchMetricHeaders.NowNs() - header.SentTimestampNs) / 1000.0;
            latencySumUs += latencyUs;
            if (samples.Count < 200000) samples.Add(latencyUs);
            completed++;
        }
        var elapsed = Stopwatch.GetElapsedTime(started).TotalSeconds;
        using var http = new System.Net.Http.HttpClient();
        var target = await http.GetFromJsonAsync<BenchServerSnapshot>(statsUrl + "/bench/stats", deadline.Token)
            ?? throw new InvalidOperationException("Missing target statistics.");
        if (target.Errors != 0 || target.ActiveMessages != completed)
            throw new InvalidOperationException($"Target mismatch: source={completed}, target={target.ActiveMessages}, errors={target.Errors}.");
        samples.Sort();
        var result = new
        {
            diagnosticOnly = true, stage, requestBytes = 64, responseBytes = 4096,
            warmupSeconds = warmup, activeSeconds = duration, elapsedSeconds = elapsed,
            sourceServerGc = System.Runtime.GCSettings.IsServerGC,
            submitted = completed, completed, errors = 0, target,
            throughputPerSecond = completed / (double)duration,
            meanUs = latencySumUs / completed,
            p50Us = samples[samples.Count / 2], p99Us = samples[(samples.Count - 1) * 99 / 100],
            limitation = "Common diagnostic source loop; compare core/full endpoints with normal run_local controls, not interchangeable report rows."
        };
        var output = Value(args, "--output", "feature-ramp.json");
        await File.WriteAllTextAsync(output, JsonSerializer.Serialize(result, new JsonSerializerOptions { WriteIndented = true }));
        Console.WriteLine(JsonSerializer.Serialize(result));
        return 0;
    }

    private static void MeasureIdleReadiness()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var target = context.CreateRouterSocket();
        using var source = context.CreateDealerSocket();
        target.SetRoutingId(RoutingId.From("idle-readiness-target"));
        target.Bind("tcp://127.0.0.1:0");
        source.Connect(target.Options.LastEndpoint);
        using var payload = Message.From(new byte[] { 42 });
        source.Send().Message(payload).Submit();
        using var received = Received.Create();
        if (!target.Recv(received) || received.FirstPart().AsReadOnlySpan()[0] != 42)
            throw new InvalidOperationException("Idle readiness probe did not establish its data connection.");
        foreach (var mask in new[]
        {
            PollEventFlags.PollIn | PollEventFlags.PollErr,
            PollEventFlags.PollIn | PollEventFlags.PollErr | PollEventFlags.PollCompletion,
            PollEventFlags.PollIn | PollEventFlags.PollErr | PollEventFlags.PollCompletion | PollEventFlags.PollOut
        })
        {
            using var poller = Systems.Zlink.Zlink.CreatePoller();
            poller.Add(target, mask, 1);
            var events = new PollEvent[1];
            var counts = new Dictionary<string, int>();
            var started = Stopwatch.GetTimestamp();
            for (var index = 0; index < 64; index++)
            {
                var ready = poller.Wait(events, TimeSpan.FromMilliseconds(5));
                var key = ready == 0 ? "timeout" : events[0].Revents.ToString();
                counts[key] = counts.GetValueOrDefault(key) + 1;
            }
            Console.WriteLine(JsonSerializer.Serialize(new
            {
                diagnosticOnly = true, connectedIdle = true, mask = mask.ToString(), iterations = 64,
                elapsedMs = Stopwatch.GetElapsedTime(started).TotalMilliseconds, counts
            }));
        }
    }

    private static async Task RunBenchBaseline(string endpoint, string commandEndpoint, string statsUrl)
    {
        // Use the existing benchmark Client loops for all three patterns. This
        // is a stripped diagnostic target, not a production Framework result.
        Console.WriteLine($"BENCH_DIAGNOSTIC_STAGE={BenchStage}");
#if ZLINK_FIXED_CODEC_BENCH
        Console.WriteLine($"BENCH_DIAGNOSTIC_FIXED_CODEC={FixedCodecName}");
#endif
        Console.WriteLine($"BENCH_DIAGNOSTIC_NO_DEADLINE={BenchWithoutDeadline}");
        Console.WriteLine($"BENCH_DIAGNOSTIC_MINIMAL_HEADER_READ={BenchMinimalHeaderRead}");
        Console.WriteLine($"BENCH_DIAGNOSTIC_SERVER_MINIMAL_HEADER_READ={BenchServerMinimalHeaderRead}");
        Console.WriteLine($"BENCH_DIAGNOSTIC_SERVER_REPLY_HEADER_REFERENCE={BenchServerReplyHeaderReference}");
        Console.WriteLine($"BENCH_DIAGNOSTIC_DIRECT_BODY_OWNER={BenchDirectBodyOwner}");
        Console.WriteLine($"BENCH_DIAGNOSTIC_PERMIT_BATCH_SIZE={BenchPermitBatchSize}");
        Console.WriteLine($"BENCH_DIAGNOSTIC_INGRESS_YIELD_INTERVAL={BenchIngressYieldInterval}");
        Console.WriteLine($"BENCH_DIAGNOSTIC_INLINE_MAILBOX={BenchInlineMailbox}");
        Console.WriteLine($"BENCH_DIAGNOSTIC_FRESH_RECEIVED={BenchFreshReceived}");
        Console.WriteLine($"BENCH_DIAGNOSTIC_MAILBOX_WORKERS={BenchMailboxWorkers}");
        Console.WriteLine($"BENCH_DIAGNOSTIC_READINESS_WAIT={BenchReadinessWait}");
        if (BenchPermitBatchSize is not (0 or 1 or 64)
            || (BenchPermitBatchSize != 0 && BenchStage != "wire"))
            throw new ArgumentException("Permit isolation requires wire stage and batch size 0, 1, or 64.");
        if (BenchIngressYieldInterval is not (0 or 1 or 64)
            || (BenchIngressYieldInterval != 0 && (BenchStage != "wire" || BenchPermitBatchSize != 0)))
            throw new ArgumentException("Yield isolation requires wire stage, interval 0, 1, or 64 and permits off.");
        if (BenchInlineMailbox && (BenchStage != "wire" || BenchIngressYieldInterval != 0))
            throw new ArgumentException("Mailbox requires wire stage with ingress yield off.");
        if (BenchMailboxWorkers < 0 || BenchMailboxWorkers > Environment.ProcessorCount
            || (BenchMailboxWorkers != 0 && (!BenchInlineMailbox || !BenchFreshReceived)))
            throw new ArgumentException("Worker isolation requires mailbox, per-record owner, and workers <= CPU count.");
        var builder = WebApplication.CreateBuilder(Array.Empty<string>());
        builder.Logging.ClearProviders();
        builder.WebHost.UseUrls(statsUrl);
        builder.Services.AddSingleton<BenchServerMetrics>();
        await using var app = builder.Build();
        var metrics = app.Services.GetRequiredService<BenchServerMetrics>();
        app.MapGet("/ready", () => Results.Ok("ready"));
        app.MapGet("/bench/stats", () => Results.Ok(metrics.Snapshot()));
        app.MapGet("/bench/diagnostic-worker", () => Results.Ok(new
        {
            frequency = Stopwatch.Frequency,
            receiveTicks = Interlocked.Read(ref WorkerReceiveTicks), receiveCount = Interlocked.Read(ref WorkerReceiveCount),
            enqueueTicks = Interlocked.Read(ref WorkerEnqueueTicks), enqueueCount = Interlocked.Read(ref WorkerEnqueueCount),
            claimTicks = Interlocked.Read(ref WorkerClaimTicks), claimCount = Interlocked.Read(ref WorkerClaimCount),
            replyTicks = Interlocked.Read(ref WorkerReplyTicks), replyCount = Interlocked.Read(ref WorkerReplyCount),
            waitTicks = Interlocked.Read(ref WorkerWaitTicks), waitCount = Interlocked.Read(ref WorkerWaitCount),
            commandWaitTicks = Interlocked.Read(ref WorkerCommandWaitTicks),
            commandWaitCount = Interlocked.Read(ref WorkerCommandWaitCount),
            pendingBytes = Interlocked.Read(ref WorkerPendingBytes)
        }));
        app.MapPost("/bench/reset", () => { metrics.Reset(); return Results.Ok(); });
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var request = context.CreateRouterSocket();
        using var command = context.CreateRouterSocket();
        request.SetRoutingId(RoutingId.From(BenchRoutingIds.RawRequestServer));
        command.SetRoutingId(RoutingId.From(BenchRoutingIds.RawCommandServer));
        request.Bind(endpoint);
        command.Bind(commandEndpoint);
        using var jobs = BenchPermitBatchSize == 0 ? null : Queue("permits");
        var requestReceiver = Task.Run(() => BenchMailboxWorkers == 0
            ? RunBaselineRouter(request, metrics, true, jobs)
            : RunWorkerRouter(request, metrics, true, jobs, app.Lifetime.ApplicationStopping));
        var commandReceiver = Task.Run(() => BenchMailboxWorkers == 0
            ? RunBaselineRouter(command, metrics, false, jobs)
            : RunWorkerRouter(command, metrics, false, jobs, app.Lifetime.ApplicationStopping));
        // A receiver fault must terminate the fixture, not produce a false
        // zero-error send result while its metrics HTTP server keeps running.
        var server = app.RunAsync();
        if (BenchMailboxWorkers == 0)
            await await Task.WhenAny(server, requestReceiver, commandReceiver);
        else
        {
            await Task.WhenAny(server, requestReceiver, commandReceiver);
            await app.StopAsync();
            await Task.WhenAll(server, requestReceiver, commandReceiver);
        }
    }

    private static async Task RunBaselineRouter(IRouterSocket router, BenchServerMetrics metrics, bool replies,
        ZLinkApplicationJobQueue? jobs)
    {
        using var received = Received.Create();
        using var poller = BenchReadinessWait ? Systems.Zlink.Zlink.CreatePoller() : null;
        var events = poller is null ? null : new PollEvent[1];
        poller?.Add(router, PollEventFlags.PollIn | PollEventFlags.PollErr, 1);
        var replyTemplate = BenchServerReplyHeaderReference && replies && BenchStage == "wire"
            ? CreateReplyHeaderReferenceTemplate() : ((byte[] Prefix, byte[] Suffix)?)null;
        var recordsSinceYield = 0;
        long queuedBytes = 0;
        var mailbox = BenchInlineMailbox ? new ZLinkMeshNodeOwnedMailbox(
            bytes => Interlocked.Add(ref queuedBytes, checked((long)bytes)),
            bytes => Interlocked.Add(ref queuedBytes, -checked((long)bytes))) : null;
        using var receiveBatch = mailbox is null ? null : new MeshReceiveBatch();
        try
        {
        while (true)
        {
            using var budget = jobs?.TryAcquireBatch(BenchPermitBatchSize);
            if (jobs is not null && budget is null)
                throw new InvalidOperationException("Inline permit fixture unexpectedly exhausted capacity.");
            var count = jobs is null ? 1 : budget!.ReservedPermitCount;
            for (var index = 0; index < count; index++)
            {
                if (poller is not null && poller.Wait(events!, TimeSpan.FromMilliseconds(1000)) == 0)
                    continue;
                using var admission = jobs is null ? null
                    : BenchPermitBatchSize == 1 ? budget : budget!.TakeReserved();
                using var freshReceived = BenchFreshReceived ? Received.Create() : null;
                if (!RunBaselineRecord(router, metrics, replies, freshReceived ?? received, replyTemplate, admission, mailbox, receiveBatch))
                    continue;
                if (BenchIngressYieldInterval != 0 && ++recordsSinceYield == BenchIngressYieldInterval)
                {
                    recordsSinceYield = 0;
                    await Task.Yield();
                }
            }
        }
        }
        finally { mailbox?.Dispose(); }
    }

    private static bool RunBaselineRecord(IRouterSocket router, BenchServerMetrics metrics, bool replies,
        Received received, (byte[] Prefix, byte[] Suffix)? replyTemplate,
        ZLinkApplicationJobQueueLease? admission,
        ZLinkMeshNodeOwnedMailbox? mailbox, MeshReceiveBatch? receiveBatch)
    {
            try
            {
                if (!router.Recv(received, BenchReadinessWait ? RecvFlags.DontWait : RecvFlags.None)) return false;
            }
            catch (ZlinkRecvException error) when (error.Result == ZlinkRecvException.ErrorCode.NoData)
            {
                // Match the Core bench's normal idle RCVTIMEO handling.
                return false;
            }
            try
            {
            admission?.MarkQueued();
            using var invocation = admission is null ? null
                : ZLinkApplicationJobQueueInvocation.Enter(admission);
            var body = received.IsSinglePart ? received.FirstPart() : received.Parts[^1];
            var envelope = BenchStage is "envelope" or "envelope-target"
                ? ZLinkEnvelopeCodec.DecodeHeader(received.FirstPart()) : null;
            ulong correlation = 1;
            ReadOnlySpan<byte> requestHeaderBytes = default;
            BenchPayload payload;
            if (BenchStage is "wire" or "wire-codec")
            {
                if (received.Parts.Count != 2
                    || !ZLinkServiceWireCodec.TryDecodeApplication(received.FirstPart().AsReadOnlySpan(), out var application, out _)
                    || application.Command != (replies ? ServiceWireConstants.Command.NodeRequest : ServiceWireConstants.Command.NodeSend)
                    || !ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(body, out var view))
                    throw new InvalidOperationException("Malformed diagnostic application wire.");
                correlation = application.Correlation;
                requestHeaderBytes = view.GetSpan(0);
                if (mailbox is not null)
                {
                    // Real queue/claim/record ownership, on the same thread.
                    // Peer classification and worker wake/handoff remain off.
                    var record = new MeshReceiveRecord(
                        replies ? MeshRecordKind.NodeRequest : MeshRecordKind.NodeSend,
                        MeshReadyDomains.Application, default, string.Empty, 0, default,
                        replies ? new MeshOperationId(0, correlation) : default,
                        replies ? MeshOperationKind.NodeRequest : default,
                        null, null, null, 0, view.Count, 0, 0, null,
                        reply: BenchFreshReceived && replies ? CaptureWireReply(received) : null,
                        applicationPayloadView: view);
                    var queued = new ZLinkMeshQueuedRecord(record, [],
                        applicationPayloadBytes: checked((ulong)view.GetSpan(1).Length),
                        payloadOwner: admission is null ? received
                            : new ZLinkApplicationJobQueueRecordOwner(received, admission));
                    if (!mailbox.TryEnqueue(queued))
                    {
                        queued.Dispose();
                        throw new InvalidOperationException("Inline mailbox rejected a record.");
                    }
                    if (!mailbox.TryClaim(admission is not null, true, out _, out _)
                        || !mailbox.Drain(receiveBatch!, 64) || receiveBatch!.Count != 1)
                        throw new InvalidOperationException("Inline mailbox did not deliver its record.");
                    view = receiveBatch[0].ApplicationPayloadView
                        ?? throw new InvalidOperationException("Mailbox lost its payload view.");
                }
                HandleWireRecord(view, correlation, received, metrics, replies, replyTemplate,
                    mailbox is null ? null : receiveBatch![0].CaptureReplyRoute());
                return true;
            }
            else
                payload = envelope is null ? DecodeBenchPayload(body)
                    : (BenchPayload)ZLinkEnvelopeCodec.DecodeBody(body, typeof(BenchPayload), envelope.ContentType, Codecs)!;
            if (admission is not null)
                ZLinkApplicationJobQueueInvocation.ReleaseForHandlerStart();
            if (!replies) { metrics.Record(payload); return true; }
            metrics.RecordReceived(payload);
            var response = BenchMetricHeaders.CreateResponsePayload(payload);
            var responseHeaderOverride = replyTemplate is { } template
                ? EncodeReplyHeaderReference(template, requestHeaderBytes) : null;
            var encoded = EncodeBenchMessage(response, response: true, requestHeader: envelope,
                correlation: correlation, headerOverride: responseHeaderOverride);
            using var header = encoded.Header;
            using var replyBody = encoded.Body;
            if (received.ReplyToken is not null)
                received.Reply().Message(header).Message(replyBody).Submit();
            else
                received.Send().Message(header).Message(replyBody).Submit();
            return true;
            }
            finally
            {
                if (mailbox is not null)
                {
                    var residue = mailbox.Release();
                    receiveBatch!.Reset();
                    if (residue)
                        throw new InvalidOperationException("Inline mailbox retained unhandled residue.");
                }
            }
    }

    private static void HandleWireRecord(ZLinkMultipartPayloadView view, ulong correlation,
        Received? received, BenchServerMetrics metrics, bool replies,
        (byte[] Prefix, byte[] Suffix)? replyTemplate = null,
        Func<IReadOnlyList<Message>, SubmitResult>? reply = null)
    {
        var envelope = BenchStage == "wire"
            ? (BenchMinimalHeaderRead || BenchServerMinimalHeaderRead) && replies
                ? ReadBenchHeaderCorrelationOnly(view.GetSpan(0)) : ZLinkEnvelopeCodec.DecodeHeader(view)
            : null;
        var payload = (BenchPayload)ZLinkEnvelopeCodec.DecodeBody(view, typeof(BenchPayload),
            envelope?.ContentType ?? "application/x-protobuf", Codecs)!;
        if (BenchPermitBatchSize != 0)
            ZLinkApplicationJobQueueInvocation.ReleaseForHandlerStart();
        if (!replies) { metrics.Record(payload); return; }
        metrics.RecordReceived(payload);
        var responseHeader = replyTemplate is { } template
            ? EncodeReplyHeaderReference(template, view.GetSpan(0)) : null;
        var encoded = EncodeBenchMessage(BenchMetricHeaders.CreateResponsePayload(payload), response: true,
            requestHeader: envelope, correlation: correlation, headerOverride: responseHeader);
        using var header = encoded.Header;
        using var body = encoded.Body;
        var replyStarted = BenchWorkerTiming ? Stopwatch.GetTimestamp() : 0;
        try
        {
            if (reply is not null)
            {
                if (reply([header, body]) != SubmitResult.Ok)
                    throw new InvalidOperationException("Captured reply was not admitted.");
            }
            else if (received!.ReplyToken is not null)
                received.Reply().Message(header).Message(body).Submit();
            else
                received.Send().Message(header).Message(body).Submit();
        }
        finally
        {
            if (BenchWorkerTiming)
            {
                Interlocked.Add(ref WorkerReplyTicks, Stopwatch.GetTimestamp() - replyStarted);
                Interlocked.Increment(ref WorkerReplyCount);
            }
        }
    }

    private static Func<IReadOnlyList<Message>, SubmitResult> CaptureWireReply(Received received) => parts =>
    {
        if (received.ReplyToken is not null)
        {
            var reply = received.Reply().Message(parts[0]);
            for (var index = 1; index < parts.Count; index++) reply.Message(parts[index]);
            reply.Submit();
        }
        else
        {
            var send = received.Send().Message(parts[0]);
            for (var index = 1; index < parts.Count; index++) send.Message(parts[index]);
            send.Submit();
        }
        return SubmitResult.Ok;
    };

    // Diagnostic handoff primitives, not the complete production dispatch pump.
    // Keep actual record ownership; the compile-fixed Channel control isolates
    // only mailbox coordination, not the production dispatch policy.
    private static async Task RunWorkerRouter(IRouterSocket router, BenchServerMetrics metrics, bool replies,
        ZLinkApplicationJobQueue? jobs, CancellationToken cancellationToken
#if ZLINK_FIXED_NO_HOST_PERMIT_BENCH
        , TaskCompletionSource? producerStarted = null
#endif
        )
    {
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
#if ZLINK_FIXED_CHANNEL_HANDOFF_BENCH
        // Diagnostic control only. Existing host permits bound queued work;
        // the default BCL implementation supports a queued-prefix Count snapshot.
        var handoff = System.Threading.Channels.Channel.CreateUnbounded<ZLinkMeshQueuedRecord>();
#else
        using var signal = new SemaphoreSlim(0);
        var mailbox = new ZLinkMeshNodeOwnedMailbox(
            bytes => Interlocked.Add(ref WorkerPendingBytes, checked((long)bytes)),
            bytes => Interlocked.Add(ref WorkerPendingBytes, -checked((long)bytes)),
            _ => signal.Release());
#endif
        var workers = Enumerable.Range(0, BenchMailboxWorkers).Select(workerIndex => Task.Run(async () =>
        {
            using var batch = new MeshReceiveBatch();
            while (!stop.IsCancellationRequested)
            {
#if ZLINK_FIXED_CHANNEL_HANDOFF_BENCH
                if (!await handoff.Reader.WaitToReadAsync(stop.Token).ConfigureAwait(false)) break;
                // Snapshot only existing backlog. Never wait to fill a batch.
                var prefix = Math.Min(64, handoff.Reader.Count);
#else
                await signal.WaitAsync(stop.Token).ConfigureAwait(false);
                var claimStarted = BenchWorkerTiming ? Stopwatch.GetTimestamp() : 0;
                var claimed = mailbox.TryClaim(jobs is not null, true, out _, out _);
                if (BenchWorkerTiming)
                {
                    Interlocked.Add(ref WorkerClaimTicks, Stopwatch.GetTimestamp() - claimStarted);
                    Interlocked.Increment(ref WorkerClaimCount);
                }
                if (!claimed) continue;
#endif
                try
                {
#if ZLINK_FIXED_CHANNEL_HANDOFF_BENCH
                    for (var index = 0; index < prefix; index++)
                    {
                        if (!handoff.Reader.TryPeek(out var candidate)
                            || !batch.CanAdd(checked((long)candidate.PayloadBytes))) break;
                        if (!handoff.Reader.TryRead(out var queued))
                            throw new InvalidOperationException("Channel control lost its queued prefix.");
                        Interlocked.Add(ref WorkerPendingBytes, -checked((long)queued.PendingBytes));
                        batch.Add(queued.Record, queued.TakeParts(), queued.TakePayloadOwner());
                    }
                    if (batch.Count == 0)
                        throw new InvalidOperationException("Channel control delivered no records.");
#else
                    if (!mailbox.Drain(batch, 64))
                        throw new InvalidOperationException("Worker claim delivered no records.");
#endif
                    for (var index = 0; index < batch.Count; index++)
                    {
                        var record = batch[index];
                        using var owner = batch.TakePayloadOwner(index);
                        if (record.ApplicationPayloadView is not { } view)
                            throw new InvalidOperationException("Worker lost the received owner or payload view.");
                        using var admission = owner is ZLinkApplicationJobQueueRecordOwner { Admission: { } lease }
                            ? ZLinkApplicationJobQueueInvocation.Enter(lease) : null;
                        HandleWireRecord(view, record.OperationId.Low, owner as Received, metrics, replies,
                            reply: record.CaptureReplyRoute());
                    }
                }
                finally
                {
                    batch.Reset();
#if !ZLINK_FIXED_CHANNEL_HANDOFF_BENCH
                    mailbox.Release();
#endif
                }
            }
        })).ToArray();
#if ZLINK_FIXED_DEDICATED_INGRESS_BENCH
        // Scheduling isolation only: preserve blocking receive and all ownership
        // semantics, but keep the entire ingress loop off the shared ThreadPool.
        var producer = Task.Factory.StartNew(() =>
#else
        var producer = Task.Run(async () =>
#endif
        {
            using var poller = BenchReadinessWait ? Systems.Zlink.Zlink.CreatePoller() : null;
            var events = poller is null ? null : new PollEvent[1];
            poller?.Add(router, PollEventFlags.PollIn | PollEventFlags.PollErr, 1);
#if ZLINK_FIXED_NO_HOST_PERMIT_BENCH
            // Once-only lifecycle observation, never an ingress/capacity gate.
            producerStarted?.SetResult();
#endif
            while (!stop.IsCancellationRequested)
            {
                if (poller is not null)
                {
                    var waitStarted = BenchWorkerTiming ? Stopwatch.GetTimestamp() : 0;
                    int ready;
                    try { ready = poller.Wait(events!, TimeSpan.FromMilliseconds(1000)); }
                    finally
                    {
                        if (BenchWorkerTiming)
                        {
                            var elapsed = Stopwatch.GetTimestamp() - waitStarted;
                            if (replies)
                            {
                                Interlocked.Add(ref WorkerWaitTicks, elapsed);
                                Interlocked.Increment(ref WorkerWaitCount);
                            }
                            else
                            {
                                Interlocked.Add(ref WorkerCommandWaitTicks, elapsed);
                                Interlocked.Increment(ref WorkerCommandWaitCount);
                            }
                        }
                    }
                    if (ready == 0) continue;
                }
#if ZLINK_FIXED_DEDICATED_INGRESS_BENCH
                // No external gate is held. Complete reservation before receive;
                // awaiting here would move the dedicated owner to the ThreadPool.
                var reservation = jobs?.AcquireAsync(stop.Token);
                var admission = reservation is not { } pending ? null
                    : pending.IsCompletedSuccessfully ? pending.GetAwaiter().GetResult()
                    : pending.AsTask().GetAwaiter().GetResult();
#else
                var admission = jobs is null ? null : await jobs.AcquireAsync(stop.Token).ConfigureAwait(false);
#endif
                var received = Received.Create();
                var transferred = false;
                try
                {
                    var receiveStarted = BenchWorkerTiming ? Stopwatch.GetTimestamp() : 0;
                    try { if (!router.Recv(received, BenchReadinessWait ? RecvFlags.DontWait : RecvFlags.None)) continue; }
                    catch (ZlinkRecvException error) when (error.Result == ZlinkRecvException.ErrorCode.NoData)
                    { continue; }
                    catch (ZlinkRecvException error) when (stop.IsCancellationRequested
                        && error.Result == ZlinkRecvException.ErrorCode.Terminated)
                    { break; }
                    catch (ObjectDisposedException) when (stop.IsCancellationRequested)
                    { break; }
                    finally
                    {
                        if (BenchWorkerTiming)
                        {
                            Interlocked.Add(ref WorkerReceiveTicks, Stopwatch.GetTimestamp() - receiveStarted);
                            Interlocked.Increment(ref WorkerReceiveCount);
                        }
                    }
                    if (received.Parts.Count != 2
                        || !ZLinkServiceWireCodec.TryDecodeApplication(received.FirstPart().AsReadOnlySpan(), out var application, out _)
                        || application.Command != (replies ? ServiceWireConstants.Command.NodeRequest : ServiceWireConstants.Command.NodeSend)
                        || !ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(received.Parts[^1], out var view))
                        throw new InvalidOperationException("Malformed worker diagnostic wire.");
                    var record = new MeshReceiveRecord(
                        replies ? MeshRecordKind.NodeRequest : MeshRecordKind.NodeSend,
                        MeshReadyDomains.Application, default, string.Empty, 0, default,
                        new MeshOperationId(0, application.Correlation),
                        replies ? MeshOperationKind.NodeRequest : default,
                        null, null, null, 0, view.Count, 0, 0, null,
                        reply: replies ? CaptureWireReply(received) : null, applicationPayloadView: view);
                    admission?.MarkQueued();
                    var queued = new ZLinkMeshQueuedRecord(record, [],
                        applicationPayloadBytes: checked((ulong)view.GetSpan(1).Length),
                        payloadOwner: admission is null ? received
                            : new ZLinkApplicationJobQueueRecordOwner(received, admission));
                    var enqueueStarted = BenchWorkerTiming ? Stopwatch.GetTimestamp() : 0;
#if ZLINK_FIXED_CHANNEL_HANDOFF_BENCH
                    var charge = checked((long)queued.PendingBytes);
                    var enqueued = false;
                    // Publish credit before a woken consumer can dequeue. A
                    // rejected/throwing write never owns credit or the record.
                    Interlocked.Add(ref WorkerPendingBytes, charge);
                    try { enqueued = handoff.Writer.TryWrite(queued); }
                    finally
                    {
                        if (!enqueued)
                        {
                            Interlocked.Add(ref WorkerPendingBytes, -charge);
                            queued.Dispose();
                        }
                    }
#else
                    var enqueued = mailbox.TryEnqueue(queued);
#endif
                    if (BenchWorkerTiming)
                    {
                        Interlocked.Add(ref WorkerEnqueueTicks, Stopwatch.GetTimestamp() - enqueueStarted);
                        Interlocked.Increment(ref WorkerEnqueueCount);
                    }
                    if (!enqueued)
                    {
#if !ZLINK_FIXED_CHANNEL_HANDOFF_BENCH
                        queued.Dispose();
#endif
                        throw new InvalidOperationException("Worker mailbox rejected a record.");
                    }
                    transferred = true;
                }
                finally
                {
                    if (!transferred)
                    {
                        received.Dispose();
                        admission?.Dispose();
                    }
                }
            }
        }
#if ZLINK_FIXED_DEDICATED_INGRESS_BENCH
        , CancellationToken.None, TaskCreationOptions.LongRunning, TaskScheduler.Default);
#else
        );
#endif
        try
        {
            // WhenAll alone would hide a fault while another worker waits forever.
            await Task.WhenAny(workers.Append(producer));
        }
        finally
        {
            stop.Cancel();
            router.Dispose();
            try { await Task.WhenAll(workers.Append(producer)).ConfigureAwait(false); }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) { }
            finally
            {
#if ZLINK_FIXED_CHANNEL_HANDOFF_BENCH
                // All publishers and consumers have joined. The remaining FIFO
                // records still own their byte charge and host admission.
                handoff.Writer.TryComplete();
                while (handoff.Reader.TryRead(out var queued))
                {
                    Interlocked.Add(ref WorkerPendingBytes, -checked((long)queued.PendingBytes));
                    queued.Dispose();
                }
#else
                mailbox.Dispose();
#endif
            }
        }
    }

    internal static Message EncodeBenchPayload(BenchPayload payload)
    {
#if ZLINK_FIXED_FRAMEWORK_CODEC
        return ZLinkEnvelopeCodec.EncodeBody(payload, typeof(BenchPayload), Codecs);
#elif ZLINK_FIXED_CODEC_BENCH
        var body = new Message(payload.CalculateSize());
        try { RawWire.Encode(payload, body.AsSpan()); return body; }
        catch { body.Dispose(); throw; }
#else
        if (BenchStage is "codec" or "codec-encode" or "envelope" or "envelope-outbound" or "envelope-target" or "wire" or "wire-codec")
            return ZLinkEnvelopeCodec.EncodeBody(payload, typeof(BenchPayload), Codecs);
        var body = BenchDirectBodyOwner
            ? new Message(payload.CalculateSize()) : Message.Allocate(payload.CalculateSize());
        try { RawWire.Encode(payload, body.AsSpan()); return body; }
        catch { body.Dispose(); throw; }
#endif
    }

#if ZLINK_FIXED_FRAMEWORK_CODEC
    internal static BenchPayload DecodeBenchPayload(Message body) =>
        (BenchPayload)ZLinkEnvelopeCodec.DecodeBody(body, typeof(BenchPayload), "application/x-protobuf", Codecs)!;
#elif ZLINK_FIXED_CODEC_BENCH
    internal static BenchPayload DecodeBenchPayload(Message body) => RawWire.Decode(body.AsReadOnlySpan());
#else
    internal static BenchPayload DecodeBenchPayload(Message body) => BenchStage is "codec" or "codec-decode"
        or "envelope" or "envelope-outbound" or "envelope-target" or "wire" or "wire-codec"
        ? (BenchPayload)ZLinkEnvelopeCodec.DecodeBody(body, typeof(BenchPayload), "application/x-protobuf", Codecs)!
        : RawWire.Decode(body.AsReadOnlySpan());
#endif

#if ZLINK_FIXED_CODEC_BENCH
#if ZLINK_FIXED_DEDICATED_INGRESS_BENCH
    private static async Task VerifyWorkerLifecycle()
    {
        foreach (var scenario in new[] { "idle-cancellation", "malformed-wire", "worker-decode-fault" })
        {
            using var context = Systems.Zlink.Zlink.CreateContext();
            using var router = context.CreateRouterSocket();
            using var source = context.CreateDealerSocket();
            router.SetRoutingId(RoutingId.From("worker-lifecycle-target"));
            router.Bind("tcp://127.0.0.1:0");
            source.Connect(router.Options.LastEndpoint);
            using var stop = new CancellationTokenSource();
#if ZLINK_FIXED_NO_HOST_PERMIT_BENCH
            var producerStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var loop = RunWorkerRouter(router, new BenchServerMetrics(), false, null, stop.Token, producerStarted);
            await await Task.WhenAny(loop, producerStarted.Task);
#else
            using var jobs = Queue("permits")!;
            var loop = RunWorkerRouter(router, new BenchServerMetrics(), false, jobs, stop.Token);
            // Observe an actual producer reservation, rather than cancelling
            // before its dedicated task has started. The CLI's external watchdog
            // bounds this selfcheck; no receive timeout or runtime budget changes.
            while (jobs.GetStatus().ReservedSupplyPermits == 0)
            {
                if (loop.IsCompleted) await loop;
                await Task.Yield();
            }
#endif
            if (scenario == "idle-cancellation") stop.Cancel();
            else if (scenario == "malformed-wire")
            {
                using var invalid = Message.From(new byte[] { 0 });
                source.Send().Message(invalid).Submit();
            }
            else
            {
                var encoded = EncodeBenchMessage(new BenchPayload { Body = ByteString.CopyFrom(new byte[] { 42 }) },
                    response: false, command: true);
                using var header = encoded.Header;
                using var body = encoded.Body;
                if (!ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(body, out var view))
                    throw new InvalidOperationException("Lifecycle fixture lost its envelope.");
                // A zero Protobuf tag fails decoding in the worker, after transfer.
                body.AsSpan()[body.AsSpan().Length - view.GetSpan(1).Length] = 0;
                source.Send().Message(header).Message(body).Submit();
            }
            Exception? failure = null;
            try { await loop; }
            catch (Exception error) { failure = error; }
            var expectedOutcome = scenario switch
            {
                "idle-cancellation" => failure is null,
                "malformed-wire" => failure is InvalidOperationException
                    { Message: "Malformed worker diagnostic wire." },
                "worker-decode-fault" => failure is InvalidProtocolBufferException,
                _ => false
            };
            if (!expectedOutcome)
                throw new InvalidOperationException($"Lifecycle selfcheck failed: {scenario}.", failure);
#if !ZLINK_FIXED_NO_HOST_PERMIT_BENCH
            var status = jobs.GetStatus();
            if (status.ReservedSupplyPermits != 0 || status.QueuedApplicationJobs != 0
                || Interlocked.Read(ref WorkerPendingBytes) != 0)
#else
            if (Interlocked.Read(ref WorkerPendingBytes) != 0)
#endif
                throw new InvalidOperationException($"Lifecycle selfcheck leaked ownership: {scenario}.");
            Console.WriteLine(JsonSerializer.Serialize(new { scenario, joined = true, permits = 0,
                pendingBytes = 0, errorType = failure?.GetType().FullName }));
        }
    }
#endif

    private static void VerifyFixedCodecMessages()
    {
        foreach (var (kind, size, response, command) in new[]
        {
            ("request", 64, false, false), ("response", 4096, true, false), ("send", 4096, false, true)
        })
        {
            var bytes = new byte[size];
            bytes.AsSpan().Fill(0xab);
            var payload = new BenchPayload { Body = ByteString.CopyFrom(bytes) };
#if ZLINK_FIXED_ENVELOPE_BENCH
            var requestHeader = response ? ZLinkClientCallCodec.CreateEnvelope(
                ZLinkMessageKind.Request, "bench", "BenchPayload", DefaultRequestTimeout) : null;
#else
            ZLinkEnvelopeHeader? requestHeader = null;
#endif
            var encoded = EncodeBenchMessage(payload, response: response, command: command, requestHeader: requestHeader);
            using var header = encoded.Header;
            using var body = encoded.Body;
            ReadOnlySpan<byte> payloadBytes = body.AsReadOnlySpan();
            BenchPayload decodedPayload;
#if ZLINK_FIXED_WIRE_BENCH
            if ((response && (!ZLinkServiceWireCodec.TryDecodeReply(header.AsReadOnlySpan(), out var reply, out _)
                    || reply.Correlation != 1 || reply.TerminalResult != (int)RequestResult.Ok))
                || (!response && (!ZLinkServiceWireCodec.TryDecodeApplication(header.AsReadOnlySpan(), out var application, out _)
                    || application.Command != (command ? ServiceWireConstants.Command.NodeSend : ServiceWireConstants.Command.NodeRequest)
                    || application.Correlation != (command ? 0UL : 1UL)))
                || !ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(body, out var view)
                || view.Count != 2)
                throw new InvalidOperationException("Fixed wire changed service frame semantics.");
            var decodedHeader = ZLinkEnvelopeCodec.DecodeHeader(view);
            var expectedKind = response ? ZLinkMessageKind.Response
                : command ? ZLinkMessageKind.Command : ZLinkMessageKind.Request;
            if (decodedHeader.Kind != expectedKind || decodedHeader.ChannelName != "bench"
                || decodedHeader.ContentType != "application/x-protobuf"
                || (response && decodedHeader.CorrelationId != requestHeader!.CorrelationId)
                || (!response && decodedHeader.MessageName != "BenchPayload")
                || (!response && !command && (decodedHeader.CorrelationId is null || decodedHeader.Deadline is null))
                || (command && (decodedHeader.CorrelationId is not null || decodedHeader.Deadline is not null))
                || (response && !DecodeBenchReply([header, body]).Equals(payload)))
                throw new InvalidOperationException("Fixed wire changed envelope semantics or reply decoding.");
            payloadBytes = view.GetSpan(1);
            decodedPayload = (BenchPayload)ZLinkEnvelopeCodec.DecodeBody(view, typeof(BenchPayload),
                decodedHeader.ContentType, Codecs)!;
#elif ZLINK_FIXED_ENVELOPE_BENCH
            var decodedHeader = ZLinkEnvelopeCodec.DecodeHeader(header);
            var expectedKind = response ? ZLinkMessageKind.Response
                : command ? ZLinkMessageKind.Command : ZLinkMessageKind.Request;
            if (decodedHeader.Kind != expectedKind || decodedHeader.ChannelName != "bench"
                || decodedHeader.ContentType != "application/x-protobuf"
                || (response && decodedHeader.CorrelationId != requestHeader!.CorrelationId)
                || (!response && decodedHeader.MessageName != "BenchPayload")
                || (!response && !command && (decodedHeader.CorrelationId is null || decodedHeader.Deadline is null))
                || (command && (decodedHeader.CorrelationId is not null || decodedHeader.Deadline is not null))
                || (response && !DecodeBenchReply([header, body]).Equals(payload)))
                throw new InvalidOperationException("Fixed envelope changed header semantics or reply decoding.");
            decodedPayload = DecodeBenchPayload(body);
#else
            if (!header.AsReadOnlySpan().SequenceEqual(response ? RawResponse : RawRequest))
                throw new InvalidOperationException("Fixed codec changed raw header bytes.");
            decodedPayload = DecodeBenchPayload(body);
#endif
            if (!payloadBytes.SequenceEqual(payload.ToByteArray()) || !decodedPayload.Equals(payload))
                throw new InvalidOperationException("Fixed codec changed message bytes or decoded values.");
            Console.WriteLine(JsonSerializer.Serialize(new { fixedCodec = FixedCodecName, kind, payloadBytes = size,
                headerBytes = header.Size, bodyBytes = body.Size,
                headerSha256 = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(header.AsReadOnlySpan())),
                bodySha256 = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(body.AsReadOnlySpan())),
                payloadBodyBytes = payloadBytes.Length,
                payloadBodySha256 = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(payloadBytes)) }));
        }
    }
#endif

    internal static Message EncodeBenchHeader(bool response = false, bool command = false,
        ZLinkEnvelopeHeader? requestHeader = null)
    {
        if (BenchStage is not ("envelope" or "envelope-outbound" or "envelope-target" or "wire")
            || (response && BenchStage == "envelope-outbound"))
            return Message.From(response ? RawResponse : RawRequest);
        var envelope = response
            ? ZLinkChannelReplyWriter.CreateReplyHeader(ZLinkMessageKind.Response, "bench", requestHeader!)
            : ZLinkClientCallCodec.CreateEnvelope(command ? ZLinkMessageKind.Command : ZLinkMessageKind.Request,
                "bench", "BenchPayload", command ? null : DefaultRequestTimeout,
                includeDeadline: !BenchWithoutDeadline);
        return ZLinkEnvelopeCodec.EncodeHeader(envelope, "application/x-protobuf");
    }

    internal static (Message Header, Message Body) EncodeBenchMessage(BenchPayload payload,
        bool response = false, bool command = false, ZLinkEnvelopeHeader? requestHeader = null,
        ulong correlation = 1, Message? headerOverride = null)
    {
        var header = headerOverride ?? EncodeBenchHeader(response, command, requestHeader);
        Message? body = null;
        Message? wireHeader = null;
        Message? packed = null;
        try
        {
            body = EncodeBenchPayload(payload);
            if (BenchStage is not ("wire" or "wire-codec")) return (header, body);
            packed = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage(
                ZLinkMessageParts.Create(header, body));
            // The fixture isolates wire framing only. Operation registration
            // remains in the binding; no Framework pending table is added here.
            wireHeader = response
                ? ZLinkServiceWireCodec.EncodeReplyMessage(correlation, (int)RequestResult.Ok, 0)
                : ZLinkServiceWireCodec.EncodeApplicationMessage(command
                    ? ServiceWireConstants.Command.NodeSend : ServiceWireConstants.Command.NodeRequest,
                    command ? 0 : correlation, null, false);
            header.Dispose();
            body.Dispose();
            return (wireHeader, packed);
        }
        catch
        {
            packed?.Dispose();
            wireHeader?.Dispose();
            body?.Dispose();
            header.Dispose();
            throw;
        }
    }

    internal static BenchPayload DecodeBenchReply(IReadOnlyList<Message> parts)
    {
        if (BenchStage is "wire" or "wire-codec")
        {
            if (parts.Count != 2
                || !ZLinkServiceWireCodec.TryDecodeReply(parts[0].AsReadOnlySpan(), out var reply, out _)
                || reply.Correlation != 1 || reply.TerminalResult != (int)RequestResult.Ok
                || !ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(parts[1], out var view))
                throw new InvalidOperationException("Malformed diagnostic reply wire.");
            return BenchStage == "wire" && !BenchMinimalHeaderRead
                ? ZLinkEnvelopeReplyDecoder.Decode<BenchPayload>(parts, "Missing reply", "Request failed", Codecs,
                    applicationPayloadView: view)
                : (BenchPayload)ZLinkEnvelopeCodec.DecodeBody(view, typeof(BenchPayload), "application/x-protobuf", Codecs)!;
        }
        return BenchStage == "envelope"
            ? ZLinkEnvelopeReplyDecoder.Decode<BenchPayload>(parts, "Missing reply", "Request failed", Codecs)
            : DecodeBenchPayload(parts[^1]);
    }

    private static ZLinkEnvelopeHeader ReadBenchHeaderCorrelationOnly(ReadOnlySpan<byte> bytes)
    {
        // Not a protocol decoder: only canonical, locally generated bench
        // envelopes are used here. Native service routing and payload/sequence
        // validation are unchanged; production validation is deliberately absent.
        var reader = new Utf8JsonReader(bytes);
        if (!reader.Read() || reader.TokenType != JsonTokenType.StartObject)
            throw new InvalidOperationException("Malformed minimal-header fixture.");
        string? correlation = null;
        var complete = false;
        while (reader.Read())
        {
            if (reader.TokenType == JsonTokenType.EndObject) { complete = true; break; }
            if (reader.TokenType != JsonTokenType.PropertyName)
                throw new InvalidOperationException("Malformed minimal-header property.");
            var isCorrelation = reader.ValueTextEquals("correlationId"u8);
            if (!reader.Read()) throw new InvalidOperationException("Missing minimal-header value.");
            if (isCorrelation) correlation = reader.GetString();
            else reader.Skip();
        }
        if (!complete || reader.Read())
            throw new InvalidOperationException("Incomplete minimal-header fixture.");
        return new ZLinkEnvelopeHeader(ZLinkMessageKind.Request, "bench", "BenchPayload",
            "application/x-protobuf", correlation, null, null, null, null);
    }

    private static (byte[] Prefix, byte[] Suffix) CreateReplyHeaderReferenceTemplate()
    {
        var request = ZLinkClientCallCodec.CreateEnvelope(ZLinkMessageKind.Request,
            "bench", "BenchPayload", DefaultRequestTimeout);
        using var encoded = ZLinkEnvelopeCodec.EncodeHeader(
            ZLinkChannelReplyWriter.CreateReplyHeader(ZLinkMessageKind.Response, "bench", request), "application/x-protobuf");
        var bytes = encoded.AsReadOnlySpan();
        var (start, length) = FindCorrelationToken(bytes);
        return (bytes[..start].ToArray(), bytes[(start + length)..].ToArray());
    }

    private static Message EncodeReplyHeaderReference((byte[] Prefix, byte[] Suffix) template, ReadOnlySpan<byte> request)
    {
        var (start, length) = FindCorrelationToken(request);
        var result = new Message(checked(template.Prefix.Length + length + template.Suffix.Length));
        try
        {
            var destination = result.AsSpan();
            template.Prefix.CopyTo(destination);
            request.Slice(start, length).CopyTo(destination[template.Prefix.Length..]);
            template.Suffix.CopyTo(destination[(template.Prefix.Length + length)..]);
            return result;
        }
        catch
        {
            result.Dispose();
            throw;
        }
    }

    private static (int Start, int Length) FindCorrelationToken(ReadOnlySpan<byte> bytes)
    {
        var reader = new Utf8JsonReader(bytes);
        if (!reader.Read() || reader.TokenType != JsonTokenType.StartObject)
            throw new InvalidOperationException("Malformed reply reference fixture.");
        while (reader.Read() && reader.TokenType == JsonTokenType.PropertyName)
        {
            var match = reader.ValueTextEquals("correlationId"u8);
            if (!reader.Read()) break;
            if (match && reader.TokenType == JsonTokenType.String)
                return (checked((int)reader.TokenStartIndex), checked((int)(reader.BytesConsumed - reader.TokenStartIndex)));
            reader.Skip();
        }
        throw new InvalidOperationException("Reply reference fixture requires a string correlation id.");
    }

    private static async Task MeasureMailboxCost(string output, int iterations, int recordsPerTurn)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(iterations);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(recordsPerTurn);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(recordsPerTurn, ZLinkReceiveBatchBudget.MaximumRecords);
        var mailbox = new ZLinkMeshNodeOwnedMailbox(static _ => { }, static _ => { });
        using var batch = new MeshReceiveBatch();
        try
        {
            for (var index = 0; index < Math.Max(1, 10000 / recordsPerTurn); index++)
                Turn(mailbox, batch, recordsPerTurn);
            var allocatedBefore = GC.GetAllocatedBytesForCurrentThread();
            var started = Stopwatch.GetTimestamp();
            for (var index = 0; index < iterations; index++)
                Turn(mailbox, batch, recordsPerTurn);
            var elapsed = Stopwatch.GetElapsedTime(started);
            var allocated = GC.GetAllocatedBytesForCurrentThread() - allocatedBefore;
            await File.WriteAllTextAsync(output, JsonSerializer.Serialize(new
            {
                operation = "same-thread-mailbox-turn", iterations, recordsPerTurn,
                nanosecondsPerOperation = elapsed.TotalNanoseconds / iterations,
                managedBytesPerOperation = (double)allocated / iterations,
                nanosecondsPerRecord = elapsed.TotalNanoseconds / iterations / recordsPerTurn,
                managedBytesPerRecord = (double)allocated / iterations / recordsPerTurn
            }, new JsonSerializerOptions { WriteIndented = true }));
        }
        finally { mailbox.Dispose(); }

        static void Turn(ZLinkMeshNodeOwnedMailbox mailbox, MeshReceiveBatch batch, int recordsPerTurn)
        {
            // Queue-only cost fixture: no socket reads or delayed live delivery.
            for (var index = 0; index < recordsPerTurn; index++)
                if (!mailbox.TryEnqueue(new ZLinkMeshQueuedRecord(default, [], applicationPayloadBytes: 0)))
                    throw new InvalidOperationException("Mailbox cost fixture rejected a record.");
            if (!mailbox.TryClaim(false, true, out _, out _)
                || !mailbox.Drain(batch, 64) || batch.Count != recordsPerTurn || mailbox.Release())
                throw new InvalidOperationException("Mailbox cost fixture did not complete one FIFO record.");
            batch.Reset();
        }
    }

    private static async Task MeasurePermitCost(string output, int iterations)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(iterations);
        using var jobs = Queue("permits")!;
        for (var warmup = 0; warmup < 10000; warmup++) Turn(jobs);
        var allocatedBefore = GC.GetAllocatedBytesForCurrentThread();
        var started = Stopwatch.GetTimestamp();
        for (var index = 0; index < iterations; index++) Turn(jobs);
        var elapsed = Stopwatch.GetElapsedTime(started);
        var allocated = GC.GetAllocatedBytesForCurrentThread() - allocatedBefore;
        await File.WriteAllTextAsync(output, JsonSerializer.Serialize(new
        {
            diagnosticOnly = true, synchronousNoIo = true, operation = "permit-handler-turn", iterations,
            nanosecondsPerOperation = elapsed.TotalNanoseconds / iterations,
            managedBytesPerOperation = (double)allocated / iterations
        }, new JsonSerializerOptions { WriteIndented = true }));

        static void Turn(ZLinkApplicationJobQueue jobs)
        {
            using var lease = jobs.TryAcquireBatch(1)
                ?? throw new InvalidOperationException("Permit cost fixture exhausted capacity.");
            lease.MarkQueued();
            using var invocation = ZLinkApplicationJobQueueInvocation.Enter(lease);
            ZLinkApplicationJobQueueInvocation.ReleaseForHandlerStart();
            if (!lease.IsReleased)
                throw new InvalidOperationException("Permit cost fixture retained its handler permit.");
        }
    }

    private static async Task MeasureStateLaneCost(string output, int iterations)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(iterations);
        await using var lane = new ZLinkStateLane();
        Func<int> work = static () => 1;
        for (var index = 0; index < 10000; index++)
            lane.RunAsync(work).GetAwaiter().GetResult();
        var allocatedBefore = GC.GetAllocatedBytesForCurrentThread();
        var started = Stopwatch.GetTimestamp();
        var total = 0;
        for (var index = 0; index < iterations; index++)
            total += lane.RunAsync(work).GetAwaiter().GetResult();
        var elapsed = Stopwatch.GetElapsedTime(started);
        var allocated = GC.GetAllocatedBytesForCurrentThread() - allocatedBefore;
        if (total != iterations)
            throw new InvalidOperationException("State lane lost a synchronous turn.");
        await File.WriteAllTextAsync(output, JsonSerializer.Serialize(new
        {
            operation = "idle-state-lane-turn", iterations,
            nanosecondsPerOperation = elapsed.TotalNanoseconds / iterations,
            managedBytesPerOperation = (double)allocated / iterations
        }, new JsonSerializerOptions { WriteIndented = true }));
    }

    private static async Task MeasureWireCost(string output, int iterations)
    {
        var rows = new List<object>();
        foreach (var size in new[] { 64, 4096 })
        {
            var payload = BenchMetricHeaders.CreatePayload(size, 1, BenchPhase.Active, 1);
            var request = ZLinkClientCallCodec.CreateEnvelope(ZLinkMessageKind.Request, "bench", "BenchPayload", DefaultRequestTimeout,
                includeDeadline: !BenchWithoutDeadline);
            using var header = ZLinkEnvelopeCodec.EncodeHeader(size == 64 ? request
                : ZLinkChannelReplyWriter.CreateReplyHeader(ZLinkMessageKind.Response, "bench", request), "application/x-protobuf");
            using var body = ZLinkEnvelopeCodec.EncodeBody(payload, typeof(BenchPayload), Codecs);
            IReadOnlyList<Message> parts = [header, body];
            using var input = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage(parts);
            using var copyDestination = Message.Allocate(input.Size);
            input.AsReadOnlySpan().CopyTo(copyDestination.AsSpan());
            if (!copyDestination.AsReadOnlySpan().SequenceEqual(input.AsReadOnlySpan()))
                throw new InvalidOperationException("Preallocated copy reference changed wire bytes.");
            using (var productionParts = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage(
                ZLinkMessageParts.Create(header, body)))
                if (!productionParts.AsReadOnlySpan().SequenceEqual(input.AsReadOnlySpan()))
                    throw new InvalidOperationException("Production part container differs from the array reference wire bytes.");
            // Reference only: reuse the encoded framing/header prefix and
            // write the typed protobuf directly into the final native owner.
            var bodyOffset = input.Size - body.Size;
            var prefix = input.AsReadOnlySpan()[..bodyOffset].ToArray();
            Message EncodeFusedReference()
            {
                var encoded = Message.Allocate(input.Size);
                try
                {
                    var destination = encoded.AsSpan();
                    prefix.CopyTo(destination);
                    payload.WriteTo(destination[bodyOffset..]);
                    return encoded;
                }
                catch { encoded.Dispose(); throw; }
            }
            using (var reference = EncodeFusedReference())
                if (!reference.AsReadOnlySpan().SequenceEqual(input.AsReadOnlySpan()))
                    throw new InvalidOperationException("Fused reference differs from the Framework wire bytes.");
            Message EncodeDirectBody(bool pooled)
            {
                var encoded = pooled ? Message.Allocate(payload.CalculateSize())
                    : new Message(payload.CalculateSize());
                try { payload.WriteTo(encoded.AsSpan()); return encoded; }
                catch { encoded.Dispose(); throw; }
            }
            foreach (var pooled in new[] { false, true })
            {
                using var reference = EncodeDirectBody(pooled);
                if (!reference.AsReadOnlySpan().SequenceEqual(body.AsReadOnlySpan()))
                    throw new InvalidOperationException("Allocator reference differs from the registered codec bytes.");
            }
            var service = size == 64
                ? ZLinkServiceWireCodec.EncodeApplication(ServiceWireConstants.Command.NodeRequest, 1, null, false)
                : ZLinkServiceWireCodec.EncodeReply(1, (int)RequestResult.Ok, 0);
            foreach (var (name, operation) in new (string, Action)[]
            {
                ("body-owner-create-dispose-pooled-reference", () => { using var owner = Message.Allocate(body.Size); }),
                ("body-owner-create-dispose-direct-reference", () => { using var owner = new Message(body.Size); }),
                ("frame-owner-create-dispose-pooled-reference", () => { using var owner = Message.Allocate(input.Size); }),
                ("frame-owner-create-dispose-direct-reference", () => { using var owner = new Message(input.Size); }),
                ("header-copy-preallocated-reference", () => header.AsReadOnlySpan().CopyTo(copyDestination.AsSpan())),
                ("body-copy-preallocated-reference", () => body.AsReadOnlySpan().CopyTo(copyDestination.AsSpan()[bodyOffset..])),
                ("frame-copy-preallocated-reference", () => input.AsReadOnlySpan().CopyTo(copyDestination.AsSpan())),
                ("protobuf-write-preallocated-reference", () => payload.WriteTo(copyDestination.AsSpan()[bodyOffset..])),
                ("codec-encode-and-pack", () =>
                {
                    using var encodedBody = ZLinkEnvelopeCodec.EncodeBody(payload, typeof(BenchPayload), Codecs);
                    using var encoded = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage(
                        ZLinkMessageParts.Create(header, encodedBody));
                }),
                ("codec-encode-and-pack-array-reference", () =>
                {
                    using var encodedBody = ZLinkEnvelopeCodec.EncodeBody(payload, typeof(BenchPayload), Codecs);
                    using var encoded = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage([header, encodedBody]);
                }),
                ("raw-fused-prebuilt-prefix-reference", () => { using var encoded = EncodeFusedReference(); }),
                ("raw-body-pooled-and-pack", () =>
                {
                    using var encodedBody = EncodeDirectBody(true);
                    using var encoded = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage([header, encodedBody]);
                }),
                ("raw-body-direct-and-pack", () =>
                {
                    using var encodedBody = EncodeDirectBody(false);
                    using var encoded = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage([header, encodedBody]);
                }),
                ("multipart-encode", () => { using var encoded = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage(parts); }),
                ("multipart-decode-view", () =>
                {
                    if (!ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(input, out _))
                        throw new InvalidOperationException("Invalid wire cost fixture.");
                }),
                ("service-encode", () =>
                {
                    using var encoded = size == 64
                        ? ZLinkServiceWireCodec.EncodeApplicationMessage(ServiceWireConstants.Command.NodeRequest, 1, null, false)
                        : ZLinkServiceWireCodec.EncodeReplyMessage(1, (int)RequestResult.Ok, 0);
                }),
                ("service-decode", () =>
                {
                    var valid = size == 64
                        ? ZLinkServiceWireCodec.TryDecodeApplication(service, out _, out _)
                        : ZLinkServiceWireCodec.TryDecodeReply(service, out _, out _);
                    if (!valid) throw new InvalidOperationException("Invalid service cost fixture.");
                })
            })
            {
                for (var index = 0; index < 10000; index++) operation();
                var allocated = GC.GetAllocatedBytesForCurrentThread();
                var started = Stopwatch.GetTimestamp();
                for (var index = 0; index < iterations; index++) operation();
                rows.Add(new { size, operation = name,
                    nsPerOperation = Stopwatch.GetElapsedTime(started).TotalNanoseconds / iterations,
                    bytesPerOperation = (GC.GetAllocatedBytesForCurrentThread() - allocated) / (double)iterations });
            }
        }
        var json = JsonSerializer.Serialize(new { diagnosticOnly = true, synchronousNoIo = true, rows },
            new JsonSerializerOptions { WriteIndented = true });
        await File.WriteAllTextAsync(output, json);
        Console.WriteLine(json);
    }

    private static async Task MeasureCodecCost(string output)
    {
        const int iterations = 1000000;
        const int blocks = 10;
        var rows = new List<object>();
        if (!Codecs.TryResolveSerializer(typeof(BenchPayload), out _, out var serializer)
            || serializer is not IZLinkMessagePartSerializer partSerializer
            || serializer is not IZLinkMessageSpanDeserializer spanDeserializer)
            throw new InvalidOperationException("Codec cost fixture requires native-part/span Protobuf serializer.");
        foreach (var size in new[] { 64, 4096 })
        {
            var value = BenchMetricHeaders.CreatePayload(size, 1, BenchPhase.Active, 1);
            using var input = new Message(value.CalculateSize());
            RawWire.Encode(value, input.AsSpan());
            Action rawEncode = () =>
            {
                using var part = BenchDirectBodyOwner
                    ? new Message(value.CalculateSize()) : Message.Allocate(value.CalculateSize());
                RawWire.Encode(value, part.AsSpan());
            };
            Action codecEncode = () =>
            {
                using var part = ZLinkEnvelopeCodec.EncodeBody(value, typeof(BenchPayload), Codecs);
            };
            Action rawDecode = () => { DiagnosticScalarSink = RawWire.Decode(input.AsReadOnlySpan()).Body.Length; };
            Action codecDecode = () =>
            {
                DiagnosticScalarSink = ((BenchPayload)ZLinkEnvelopeCodec.DecodeBody(
                    input, typeof(BenchPayload), "application/x-protobuf", Codecs)!).Body.Length;
            };
            Action resolvedEncode = () => { using var part = partSerializer.SerializePart(value, typeof(BenchPayload)); };
            Action resolvedDecode = () =>
            {
                DiagnosticScalarSink = ((BenchPayload)spanDeserializer.Deserialize(
                    input.AsReadOnlySpan(), typeof(BenchPayload))!).Body.Length;
            };
            Action constructedDecode = () =>
            {
                var decoded = new BenchPayload();
                decoded.MergeFrom(input.AsReadOnlySpan());
                DiagnosticScalarSink = decoded.Body.Length;
            };
            Action resolveEncode = () =>
            {
                if (!Codecs.TryResolveSerializer(typeof(BenchPayload), out var contentType, out var found))
                    throw new InvalidOperationException("Missing codec.");
                DiagnosticScalarSink = contentType.Length + (ReferenceEquals(found, serializer) ? 1 : 0);
            };
            Action resolveDecode = () =>
            {
                if (!Codecs.TryGetSerializer("application/x-protobuf", out var found))
                    throw new InvalidOperationException("Missing codec.");
                DiagnosticScalarSink = ReferenceEquals(found, serializer) ? 1 : 0;
            };
            var operations = new[]
                { ("raw-encode", rawEncode), ("codec-encode", codecEncode),
                  ("resolved-encode", resolvedEncode), ("raw-decode", rawDecode),
                  ("codec-decode", codecDecode), ("resolved-decode", resolvedDecode),
                  ("constructed-decode", constructedDecode), ("resolve-encode", resolveEncode),
                  ("resolve-decode", resolveDecode) };
            foreach (var (_, operation) in operations)
                for (var warmup = 0; warmup < 100000; warmup++) operation();
            var elapsedTicks = new long[operations.Length];
            var allocatedBytes = new long[operations.Length];
            // Rotate equal-sized blocks so fixed operation order and JIT warmup
            // do not masquerade as the cost of the codec wrapper.
            for (var block = 0; block < blocks; block++)
            {
                for (var offset = 0; offset < operations.Length; offset++)
                {
                    var operationIndex = (offset + block) % operations.Length;
                    var operation = operations[operationIndex].Item2;
                    var allocated = GC.GetAllocatedBytesForCurrentThread();
                    var started = Stopwatch.GetTimestamp();
                    for (var index = 0; index < iterations / blocks; index++) operation();
                    elapsedTicks[operationIndex] += Stopwatch.GetTimestamp() - started;
                    allocatedBytes[operationIndex] += GC.GetAllocatedBytesForCurrentThread() - allocated;
                }
            }
            for (var index = 0; index < operations.Length; index++)
            {
                var name = operations[index].Item1;
                rows.Add(new { size, operation = name, iterations,
                    nsPerOperation = elapsedTicks[index] * (1000000000.0 / Stopwatch.Frequency) / iterations,
                    bytesPerOperation = allocatedBytes[index] / (double)iterations });
            }
        }
        var json = JsonSerializer.Serialize(new { diagnosticOnly = true, synchronousNoIo = true,
            directBodyOwner = BenchDirectBodyOwner, blocks, warmupPerOperation = 100000, rows },
            new JsonSerializerOptions { WriteIndented = true });
        await File.WriteAllTextAsync(output, json);
        Console.WriteLine(json);
    }

    private static async Task MeasureEnvelopeCost(string output, int iterations)
    {
        var rows = new List<object>();
        foreach (var kind in new[] { ZLinkMessageKind.Request, ZLinkMessageKind.Response, ZLinkMessageKind.Command })
        {
            var requestHeader = kind == ZLinkMessageKind.Response
                ? ZLinkClientCallCodec.CreateEnvelope(ZLinkMessageKind.Request, "bench", "BenchPayload", DefaultRequestTimeout)
                : null;
            var header = requestHeader is not null
                ? ZLinkChannelReplyWriter.CreateReplyHeader(kind, "bench", requestHeader)
                : ZLinkClientCallCodec.CreateEnvelope(kind, "bench", "BenchPayload",
                    kind == ZLinkMessageKind.Request ? DefaultRequestTimeout : null,
                    includeDeadline: !BenchWithoutDeadline);
            using var input = ZLinkEnvelopeCodec.EncodeHeader(header, "application/x-protobuf");
            using var requestInput = requestHeader is not null
                ? ZLinkEnvelopeCodec.EncodeHeader(requestHeader, "application/x-protobuf") : null;
            var replyTemplate = requestHeader is not null
                ? CreateReplyHeaderReferenceTemplate() : ((byte[] Prefix, byte[] Suffix)?)null;
            if (kind == ZLinkMessageKind.Request)
            {
                var full = ZLinkEnvelopeCodec.DecodeHeader(input);
                var minimal = ReadBenchHeaderCorrelationOnly(input.AsReadOnlySpan());
                using var fullEnvelopeReply = ZLinkEnvelopeCodec.EncodeHeader(
                    ZLinkChannelReplyWriter.CreateReplyHeader(ZLinkMessageKind.Response, "bench", full), "application/x-protobuf");
                using var minimalEnvelopeReply = ZLinkEnvelopeCodec.EncodeHeader(
                    ZLinkChannelReplyWriter.CreateReplyHeader(ZLinkMessageKind.Response, "bench", minimal), "application/x-protobuf");
                if (!fullEnvelopeReply.AsReadOnlySpan().SequenceEqual(minimalEnvelopeReply.AsReadOnlySpan()))
                    throw new InvalidOperationException("Minimal header changes the Framework response envelope bytes.");
            }
            foreach (var (name, operation) in new (string, Action)[]
            {
                // Lower bounds on the same validated fixture bytes. These do
                // not dispatch fields or replace production protocol validation.
                ("json-token-scan-reference", () => ReadJsonScalarsReference(input.AsReadOnlySpan(), false, false, false)),
                ("json-scalars-reference", () => ReadJsonScalarsReference(input.AsReadOnlySpan(), true, false, false)),
                ("json-scalars-strings-reference", () => ReadJsonScalarsReference(input.AsReadOnlySpan(), true, true, false)),
                ("json-scalars-strings-namecopy-reference", () => ReadJsonScalarsReference(input.AsReadOnlySpan(), true, true, true)),
                // Isolate only the existing header record allocation: all
                // strings, timestamp and metadata references are reused.
                ("header-record-allocation-reference", () => { _ = header with { }; }),
                ("create", () =>
                {
                    _ = requestHeader is not null ? ZLinkChannelReplyWriter.CreateReplyHeader(kind, "bench", requestHeader)
                        : ZLinkClientCallCodec.CreateEnvelope(kind, "bench", "BenchPayload",
                            kind == ZLinkMessageKind.Request ? DefaultRequestTimeout : null,
                            includeDeadline: !BenchWithoutDeadline);
                }),
                ("encode", () => { using var part = ZLinkEnvelopeCodec.EncodeHeader(header, "application/x-protobuf"); }),
                ("decode", () => { _ = ZLinkEnvelopeCodec.DecodeHeader(input); }),
                ("reply-create-and-encode", () =>
                {
                    if (requestHeader is null) return;
                    using var part = ZLinkEnvelopeCodec.EncodeHeader(
                        ZLinkChannelReplyWriter.CreateReplyHeader(ZLinkMessageKind.Response, "bench", requestHeader), "application/x-protobuf");
                }),
                ("reply-template-reference", () =>
                {
                    if (replyTemplate is not { } template || requestInput is null) return;
                    using var part = EncodeReplyHeaderReference(template, requestInput.AsReadOnlySpan());
                }),
                ("decode-correlation-only-reference", () =>
                {
                    _ = ReadBenchHeaderCorrelationOnly(input.AsReadOnlySpan());
                }),
                // Lower bound only: reference deserialization excludes the
                // Framework protocol and flow validation retained by decode.
                ("decode-json-reference", () =>
                {
                    _ = JsonSerializer.Deserialize<ZLinkEnvelopeHeader>(input.AsReadOnlySpan(),
                        ZLinkJsonSerializerOptions.Default);
                })
            })
            {
                if (kind != ZLinkMessageKind.Response && name.StartsWith("reply-", StringComparison.Ordinal)) continue;
                for (var warmup = 0; warmup < 10000; warmup++) operation();
                var allocated = GC.GetAllocatedBytesForCurrentThread();
                var started = Stopwatch.GetTimestamp();
                for (var index = 0; index < iterations; index++) operation();
                rows.Add(new { kind = kind.ToString(), operation = name,
                    deadlinePresent = header.Deadline.HasValue, headerBytes = input.Size,
                    nsPerOperation = Stopwatch.GetElapsedTime(started).TotalNanoseconds / iterations,
                    bytesPerOperation = (GC.GetAllocatedBytesForCurrentThread() - allocated) / (double)iterations });
            }
        }
        var json = JsonSerializer.Serialize(new { diagnosticOnly = true, synchronousNoIo = true, rows },
            new JsonSerializerOptions { WriteIndented = true });
        await File.WriteAllTextAsync(output, json);
        Console.WriteLine(json);
    }

    private static void ReadJsonScalarsReference(ReadOnlySpan<byte> bytes,
        bool scalars, bool strings, bool names)
    {
        var reader = new Utf8JsonReader(bytes);
        Span<char> nameBuffer = stackalloc char[14];
        var isDeadline = false;
        var total = 0;
        while (reader.Read())
        {
            total++;
            if (!scalars) continue;
            switch (reader.TokenType)
            {
                case JsonTokenType.PropertyName:
                    isDeadline = reader.ValueTextEquals("deadline"u8);
                    if (names) total += reader.CopyString(nameBuffer);
                    break;
                case JsonTokenType.String when isDeadline:
                    if (!reader.TryGetDateTimeOffset(out var timestamp))
                        throw new InvalidOperationException("Invalid deadline in scalar reference.");
                    total += (int)(timestamp.Ticks & 255);
                    break;
                case JsonTokenType.String:
                    total += strings ? reader.GetString()!.Length : reader.ValueSpan.Length;
                    break;
                case JsonTokenType.Number:
                    total += reader.GetInt32();
                    break;
            }
        }
        DiagnosticScalarSink = total;
    }

    private static async Task MeasurePendingCost(string output)
    {
        const int peak = 4096;
        const int iterations = 100000;
        var pending = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var freshSet = new HashSet<Task> { pending.Task };
        var grownSet = new HashSet<Task> { pending.Task };
        var grownList = new List<Task> { pending.Task };
        for (var index = 0; index < peak - 1; index++)
        {
            var completed = Task.FromResult(index);
            grownSet.Add(completed);
            grownList.Add(completed);
        }
        grownSet.RemoveWhere(static task => task.IsCompleted);
        grownList.RemoveAll(static task => task.IsCompleted);
        var rows = new List<object>();
        foreach (var (name, operation) in new (string, Action)[]
        {
            ("fresh-hashset", () => freshSet.RemoveWhere(static task => task.IsCompleted)),
            ("grown-hashset", () => grownSet.RemoveWhere(static task => task.IsCompleted)),
            ("grown-list", () => grownList.RemoveAll(static task => task.IsCompleted))
        })
        {
            for (var warmup = 0; warmup < 10000; warmup++) operation();
            var started = Stopwatch.GetTimestamp();
            for (var index = 0; index < iterations; index++) operation();
            rows.Add(new { operation = name, pendingCount = 1, previousPeak = name == "fresh-hashset" ? 1 : peak,
                nsPerScan = Stopwatch.GetElapsedTime(started).TotalNanoseconds / iterations });
        }
        var json = JsonSerializer.Serialize(new { diagnosticOnly = true, rows },
            new JsonSerializerOptions { WriteIndented = true });
        await File.WriteAllTextAsync(output, json);
        Console.WriteLine(json);
    }

    private static async Task RunTarget(string stage, string endpoint, string statsUrl)
    {
        var builder = WebApplication.CreateBuilder(Array.Empty<string>());
        builder.Logging.ClearProviders();
        builder.WebHost.UseUrls(statsUrl);
        builder.Services.AddSingleton<BenchServerMetrics>();
        if (stage == "full")
            builder.Services.AddZLinkFramework(framework =>
            {
                framework.Codecs.Use(ZLinkProtobufCodec.Default);
                framework.AddRouteMesh("bench").Listen(endpoint).SetRoutingId(TargetRid)
                    .AddRouteRequestHandler<RampEchoHandler, BenchPayload, BenchPayload>("BenchPayload");
            });
        await using var app = builder.Build();
        var metrics = app.Services.GetRequiredService<BenchServerMetrics>();
        app.MapGet("/bench/stats", () => Results.Ok(metrics.Snapshot()));
        app.MapGet("/ready", () => Results.Ok("ready"));
        if (stage == "full") { await app.RunAsync(); return; }
        using var context = Systems.Zlink.Zlink.CreateContext();
        if (stage is "managed" or "permits")
        {
            using var jobs = Queue(stage);
            await using var node = Node(context, TargetRid, endpoint, jobs);
            await using var pump = new ZLinkMeshDispatchPump(node, new ZLinkMeshCompletionTable(), jobs);
            pump.SetNodeRouteHandler((records, _) =>
            {
                foreach (var record in records)
                {
                    using var receivedOwner = record;
                    using var admission = record.ApplicationJobAdmission is { } queuedAdmission
                        ? ZLinkApplicationJobQueueInvocation.Enter(queuedAdmission)
                        : null;
                    ZLinkApplicationJobQueueInvocation.ReleaseForHandlerStart();
                    var payload = Decode(record);
                    metrics.RecordReceived(payload);
                    var reply = EncodeEnvelope(BenchMetricHeaders.CreateResponsePayload(payload), true);
                    try
                    {
                        if (record.Reply(reply) != SubmitResult.Ok)
                            throw new InvalidOperationException("Managed reply was not accepted.");
                    }
                    finally { DisposeParts(reply); }
                }
                return ValueTask.CompletedTask;
            });
            node.Start();
            pump.EnsureStarted();
            await app.RunAsync();
            return;
        }
        using var router = context.CreateRouterSocket();
        router.SetRoutingId(TargetRid);
        router.Bind(endpoint);
        // The minimal path deliberately matches ZLinkRawServer: one reusable
        // Received, blocking Recv, inline decode/metrics/reply, no managed owner.
        _ = Task.Run(() =>
        {
            using var received = Received.Create();
            while (true)
            {
                if (!router.Recv(received)) continue;
                var payload = DecodeWire(stage, received.Parts, false);
                metrics.RecordReceived(payload);
                var parts = EncodeWire(stage, BenchMetricHeaders.CreateResponsePayload(payload), true);
                try { received.Reply().Messages(parts).Submit(); }
                finally { DisposeParts(parts); }
            }
        });
        await app.RunAsync();
    }

    private sealed class Source : IAsyncDisposable
    {
        private readonly string stage;
        private IContext? context;
        private IRouterSocket? socket;
        private ZLinkManagedMeshNode? node;
        private ZLinkApplicationJobQueue? jobs;
        private IHost? host;
        private IZLinkRouteClient? client;
        private Source(string stage) => this.stage = stage;

        internal static async Task<Source> Create(string stage, string endpoint)
        {
            var source = new Source(stage);
            var self = RoutingId.From($"bench-source-{Environment.ProcessId}");
            if (stage == "full")
            {
                var builder = Host.CreateApplicationBuilder(Array.Empty<string>());
                builder.Logging.ClearProviders();
                builder.Services.AddZLinkFramework(framework =>
                {
                    framework.Codecs.Use(ZLinkProtobufCodec.Default);
                    var mesh = framework.AddRouteMesh("bench").Listen("tcp://127.0.0.1:0").SetRoutingId(self);
                    mesh.PeerConnections.Connect(TargetRid, endpoint);
                });
                source.host = builder.Build();
                await source.host.StartAsync();
                source.client = source.host.Services.GetRequiredService<IZLinkRouteClient>();
                return source;
            }
            source.context = Systems.Zlink.Zlink.CreateContext();
            if (stage is "managed" or "permits")
            {
                source.jobs = Queue(stage);
                source.node = Node(source.context, self, "tcp://127.0.0.1:0", source.jobs);
                source.node.Start();
                source.node.ConnectPeer(endpoint, TargetRid);
            }
            else
            {
                source.socket = source.context.CreateRouterSocket();
                source.socket.SetRoutingId(self);
                source.socket.Connect(endpoint);
            }
            return source;
        }

        internal async Task WaitReady(CancellationToken token)
        {
            if (host is not null)
            {
                var mesh = host.Services.GetRequiredService<ZLinkFrameworkRuntime>().GetMeshNodeRuntime("bench");
                while (mesh.Node.ClassifyMeshPeerTarget(TargetRid) != ZLinkRouteMeshTargetClassification.ReadyEligible)
                    await Task.Delay(10, token);
                return;
            }
            if (node is null) return;
            while (node.Status().AdmittedPeerCount == 0)
                await Task.Delay(10, token);
        }

        internal async ValueTask<BenchPayload> Request(BenchPayload payload, CancellationToken token)
        {
            if (client is not null)
                return await client.RequestToNode("bench", TargetRid, payload).Async<BenchPayload>(token);
            if (node is not null)
            {
                var parts = EncodeEnvelope(payload, false);
                try
                {
                    using var reply = await node.RequestToNodeDirectAsync(TargetRid, parts,
                        SendFlags.None, ReadOnlyMemory<byte>.Empty, TimeSpan.FromSeconds(30), token);
                    return Decode(reply);
                }
                finally { DisposeParts(parts); }
            }
            var messages = EncodeWire(stage, payload, false);
            try
            {
                var response = await socket!.Request(TargetRid).Messages(messages).Async(token).Reply;
                try { return DecodeWire(stage, response, true); }
                finally { DisposeParts(response); }
            }
            finally { DisposeParts(messages); }
        }

        public async ValueTask DisposeAsync()
        {
            if (host is not null) { await host.StopAsync(); host.Dispose(); }
            if (node is not null) await node.DisposeAsync();
            jobs?.Dispose();
            socket?.Dispose();
            context?.Dispose();
        }
    }

    private static ZLinkApplicationJobQueue? Queue(string stage) => stage != "permits" ? null
        : new ZLinkApplicationJobQueue(ZLinkApplicationJobQueueCapacityResolver.Resolve(
            ZLinkApplicationJobQueueProfile.Balanced, null,
            ZLinkApplicationJobQueueCapacityResolver.ResolveEffectiveProcessorCount(Environment.ProcessorCount)));

    private static ZLinkManagedMeshNode Node(IContext context, RoutingId rid, string endpoint,
        ZLinkApplicationJobQueue? jobs = null)
    {
        var node = new ZLinkManagedMeshNode(context, "bench", applicationJobQueue: jobs);
        node.SetRoutingId(rid);
        node.SetBind(endpoint);
        node.AddChannel("bench");
        return node;
    }

    private static BenchPayload Decode(ZLinkBackendRouteReceived record) =>
        record.ApplicationPayloadView is { } view
            ? (BenchPayload)ZLinkEnvelopeCodec.DecodeBody(view, typeof(BenchPayload),
                ZLinkEnvelopeCodec.DecodeHeader(view).ContentType, Codecs)!
            : (BenchPayload)ZLinkEnvelopeCodec.DecodeBody(record.Parts, typeof(BenchPayload), Codecs)!;

    private static IReadOnlyList<Message> EncodeEnvelope(BenchPayload payload, bool response) =>
        ZLinkEnvelopeCodec.EncodeParts(new ZLinkEnvelopeHeader(
            response ? ZLinkMessageKind.Response : ZLinkMessageKind.Request,
            "bench", "BenchPayload", "application/x-protobuf", "ramp", null, null, null, null),
            payload, typeof(BenchPayload), Codecs);

    private static IReadOnlyList<Message> EncodeWire(string stage, BenchPayload payload, bool response)
    {
        if (stage is "core" or "codec")
        {
            if (stage == "codec")
                return [Message.From(response ? RawResponse : RawRequest),
                    ZLinkEnvelopeCodec.EncodeBody(payload, typeof(BenchPayload), Codecs)];
            var body = Message.Allocate(payload.CalculateSize());
            RawWire.Encode(payload, body.AsSpan());
            return [Message.From(response ? RawResponse : RawRequest), body];
        }
        var parts = EncodeEnvelope(payload, response);
        if (stage == "envelope") return parts;
        try
        {
            return [response
                    ? ZLinkServiceWireCodec.EncodeReplyMessage(1, (int)RequestResult.Ok, 0)
                    : ZLinkServiceWireCodec.EncodeApplicationMessage(ServiceWireConstants.Command.NodeRequest, 1, null, false),
                ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage(parts)];
        }
        finally { DisposeParts(parts); }
    }

    private static BenchPayload DecodeWire(string stage, IReadOnlyList<Message> parts, bool response)
    {
        if (stage == "core") return RawWire.Decode(parts[^1].AsReadOnlySpan());
        if (stage == "codec") return (BenchPayload)ZLinkEnvelopeCodec.DecodeBody(
            parts[^1], typeof(BenchPayload), "application/x-protobuf", Codecs)!;
        if (stage == "envelope") return (BenchPayload)ZLinkEnvelopeCodec.DecodeBody(parts, typeof(BenchPayload), Codecs)!;
        if (parts.Count != 2 || !ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(parts[1], out var view))
            throw new InvalidOperationException("Malformed diagnostic service wire.");
        if (response)
        {
            if (!ZLinkServiceWireCodec.TryDecodeReply(parts[0].AsReadOnlySpan(), out var reply, out _)
                || reply.Correlation != 1 || reply.TerminalResult != (int)RequestResult.Ok)
                throw new InvalidOperationException("Invalid diagnostic terminal.");
        }
        else if (!ZLinkServiceWireCodec.TryDecodeApplication(parts[0].AsReadOnlySpan(), out var request, out _)
            || request.Correlation != 1)
            throw new InvalidOperationException("Invalid diagnostic request.");
        return (BenchPayload)ZLinkEnvelopeCodec.DecodeBody(view, typeof(BenchPayload),
            ZLinkEnvelopeCodec.DecodeHeader(view).ContentType, Codecs)!;
    }

    private static void Validate(BenchPayload response, BenchPayload request)
    {
        if (!BenchMetricHeaders.TryDecode(request, out var expected)
            || !BenchMetricHeaders.TryDecode(response, out var actual)
            || !BenchMetricHeaders.IsExpected(actual, expected.RunId, expected.Phase, 4096, expected.Sequence))
            throw new InvalidOperationException("Response payload mismatch.");
    }

    private static void DisposeParts(IReadOnlyList<Message> parts) { foreach (var part in parts) part.Dispose(); }
    private static byte[] RawHeader(int kind) => Encoding.UTF8.GetBytes(
        $"{{\"kind\":{kind},\"channelName\":\"bench\",\"messageName\":\"BenchPayload\",\"contentType\":\"application/x-protobuf\",\"correlationId\":null,\"deadline\":null,\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null}}");
    private static ZLinkCodecRegistryBuilder CreateCodecs() { var codecs = new ZLinkCodecRegistryBuilder(); codecs.Use(ZLinkProtobufCodec.Default); return codecs; }
    private static string Value(string[] args, string name, string fallback) { var index = Array.IndexOf(args, name); return index < 0 ? fallback : args[index + 1]; }
}

internal sealed class RampEchoHandler(BenchServerMetrics metrics) : IZLinkRouteRequestHandler<BenchPayload, BenchPayload>
{
    public ValueTask<BenchPayload> HandleAsync(BenchPayload request, ZLinkRouteMessageContext context, CancellationToken cancellationToken)
    {
        metrics.RecordReceived(request);
        return ValueTask.FromResult(BenchMetricHeaders.CreateResponsePayload(request));
    }
}
