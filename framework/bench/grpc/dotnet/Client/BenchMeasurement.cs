using System.Diagnostics;
using System.Net.Http.Json;
using System.Text;
using Systems.Zlink;
using WithGrpcBench.Shared;

namespace WithGrpcBench.Client;

internal static class BenchMeasurement
{
    internal static void ConfigureThreadPool(int submitConcurrency)
    {
        ThreadPool.GetMinThreads(out var workers, out var completionPorts);
        ThreadPool.SetMinThreads(
            Math.Max(workers, submitConcurrency + Environment.ProcessorCount * 2),
            completionPorts
        );
    }

    internal static async Task<BenchResult> RunActiveAsync(
        IBenchTransport transport,
        SourceMetrics metrics,
        BenchOptions options,
        BenchTriggerRequest trigger,
        CancellationToken stopping
    )
    {
        using var operationCancellation = CancellationTokenSource.CreateLinkedTokenSource(stopping);
        operationCancellation.CancelAfter(options.Timeout);
        using var adminCancellation = CancellationTokenSource.CreateLinkedTokenSource(stopping);
        adminCancellation.CancelAfter(options.Timeout);
        if (transport is RawBenchTransport rawTransport)
            rawTransport.StartCompletionPump();
        using var http = new HttpClient { Timeout = options.Timeout };
        using (
            var reset = await http.PostAsync(
                $"{options.TargetStatsUrl}/bench/reset",
                null,
                adminCancellation.Token
            )
        )
        {
            reset.EnsureSuccessStatusCode();
        }

        var resources = ResourceSample.Start();
        var deadline =
            Stopwatch.GetTimestamp()
            + checked((long)(Stopwatch.Frequency * (trigger.durationMs / 1000.0)));
        var send = trigger.pattern == "send-saturation";
        metrics.Reset(deadline, trigger.phase == "warmup" ? BenchPhase.Warmup : BenchPhase.Active);
        var activeAtClose = CaptureActiveAtDeadlineAsync(
            http,
            options.TargetStatsUrl,
            resources,
            deadline,
            adminCancellation.Token
        );
        var runId = HeaderRunId(trigger.runId);
        var next = new Sequence();

        switch (trigger.pattern)
        {
            case "request-serial":
                await RunRequestWorkersAsync(
                    transport,
                    metrics,
                    trigger,
                    runId,
                    next,
                    deadline,
                    1,
                    0,
                    operationCancellation.Token
                );
                break;
            case "request-window":
                await RunRequestWorkersAsync(
                    transport,
                    metrics,
                    trigger,
                    runId,
                    next,
                    deadline,
                    trigger.requestWindow,
                    0,
                    operationCancellation.Token
                );
                break;
            case "request-backpressure":
                RunRequestBackpressureStreams(
                    transport,
                    metrics,
                    trigger,
                    runId,
                    next,
                    deadline,
                    operationCancellation,
                    options.DrainBoundMs
                );
                break;
            case "send-saturation":
                await RunSendWorkersAsync(
                    transport,
                    metrics,
                    trigger,
                    runId,
                    next,
                    deadline,
                    trigger.submitConcurrency,
                    operationCancellation.Token
                );
                break;
            default:
                throw new InvalidOperationException($"Unsupported pattern {trigger.pattern}.");
        }

        metrics.FreezeTerminal();
        var elapsedSeconds = trigger.durationMs / 1000.0;
        var boundary = await activeAtClose;
        var sourceResources = boundary.Source;
        var receivedAtClose = send ? (long?)boundary.Target.Received : null;
        var target =
            await http.GetFromJsonAsync<BenchServerSnapshot>(
                $"{options.TargetStatsUrl}/bench/stats",
                adminCancellation.Token
            ) ?? BenchServerSnapshot.Empty;
        var snapshot = metrics.Result();
        var completed = snapshot.Completed;
        return new BenchResult(
            $"{options.Implementation}-{trigger.pattern}",
            send ? "KMSG/s" : "KOPS",
            trigger.payloadBytes,
            elapsedSeconds,
            snapshot.Submitted,
            completed,
            snapshot.Errors,
            target.Errors,
            options.Warmup,
            (send ? receivedAtClose!.Value : snapshot.CompletedAtClose)
                / Math.Max(0.001, elapsedSeconds),
            snapshot.MeanMicros,
            snapshot.P95Micros,
            snapshot.P99Micros,
            send ? boundary.Target.MeanMicros : null,
            send ? boundary.Target.P95Micros : null,
            send ? boundary.Target.P99Micros : null,
            sourceResources.CpuSeconds,
            sourceResources.WorkingSetMb,
            boundary.Target.CpuSeconds,
            boundary.Target.WorkingSetMb,
            snapshot.PeakInFlight,
            trigger.pattern == "request-window" ? trigger.requestWindow : null,
            snapshot.Abandoned,
            receivedAtClose,
            snapshot.CompletedAtClose,
            snapshot.ErrorSummary,
            snapshot.OtherErrors,
            sourceResources.AllocatedBytes,
            boundary.Target.AllocatedBytes,
            sourceResources.ThreadPoolWorkItems,
            boundary.Target.ThreadPoolWorkItems
        );
    }

    private static async Task<ActiveSnapshot> CaptureActiveAtDeadlineAsync(
        HttpClient http,
        string targetStatsUrl,
        ResourceSample resources,
        long deadline,
        CancellationToken cancellationToken
    )
    {
        var remaining = Stopwatch.GetElapsedTime(Stopwatch.GetTimestamp(), deadline);
        if (remaining > TimeSpan.Zero)
            await Task.Delay(remaining, cancellationToken);
        var source = resources.Finish();
        var target =
            await http.GetFromJsonAsync<BenchServerSnapshot>(
                $"{targetStatsUrl}/bench/stats",
                cancellationToken
            )
            ?? throw new InvalidOperationException(
                "Target stats were empty at the active boundary."
            );
        return new ActiveSnapshot(source, target);
    }

    private static async Task RunRequestWorkersAsync(
        IBenchTransport transport,
        SourceMetrics metrics,
        BenchTriggerRequest trigger,
        uint runId,
        Sequence next,
        long deadline,
        int workers,
        int stream,
        CancellationToken cancellationToken
    )
    {
        if (workers > 1)
        {
            var tasks = Enumerable.Range(0, workers).Select(_ => Task.Run(async () =>
            {
                while (Stopwatch.GetTimestamp() < deadline)
                {
                    var sequence = next.Next();
                    await ExecuteRequestAsync(
                        transport, metrics, trigger, runId, stream, sequence, cancellationToken
                    );
                }
            }, cancellationToken));
            await transport.DriveAsync(Task.WhenAll(tasks));
            return;
        }
        var local = new SourceMetrics[workers];
        RunDedicatedStreams(workers, index =>
        {
            var streamMetrics = local[index] = new SourceMetrics(metrics.SampleLimit);
            streamMetrics.Reset(deadline, metrics.Phase);
            while (Stopwatch.GetTimestamp() < deadline)
            {
                var sequence = next.Next();
                ExecuteRequestAsync(
                    transport,
                    streamMetrics,
                    trigger,
                    runId,
                    stream,
                    sequence,
                    cancellationToken
                ).GetAwaiter().GetResult();
            }
        });
        foreach (var streamMetrics in local)
            metrics.MergeFrom(streamMetrics);
    }

    private static void RunRequestBackpressureStreams(
        IBenchTransport transport,
        SourceMetrics metrics,
        BenchTriggerRequest trigger,
        uint runId,
        Sequence next,
        long deadline,
        CancellationTokenSource operationCancellation,
        int drainBoundMs
    )
    {
        var locals = new SourceMetrics[trigger.submitConcurrency];
        RunDedicatedStreams(trigger.submitConcurrency, stream =>
        {
            var streamMetrics = locals[stream] = new SourceMetrics(metrics.SampleLimit);
            streamMetrics.Reset(deadline, metrics.Phase);
            var pending = new List<Task>();
            var issuedSincePrune = 0;
            while (Stopwatch.GetTimestamp() < deadline)
            {
                var sequence = next.Next();
                var payload = BenchMetricHeaders.CreatePayload(
                    trigger.payloadBytes,
                    runId,
                    streamMetrics.Phase,
                    sequence
                );
                var started = streamMetrics.Begin();
                try
                {
                    Task<BenchPayload> reply;
                    Task? admitted = null;
                    if (transport is RawBenchTransport raw)
                    {
                        var submission = raw.SubmitRequest(payload, operationCancellation.Token);
                        reply = submission.Reply;
                        if (submission.Result == SubmitResult.Backpressured)
                            admitted = submission.Admitted;
                        else if (submission.Result != SubmitResult.Ok)
                            throw new InvalidOperationException(
                                $"Raw request submit returned {submission.Result}."
                            );
                    }
                    else if (transport is FrameworkBenchTransport framework)
                    {
                        var request = framework.SubmitRequest(payload, operationCancellation.Token);
                        reply = request.AsTask();
                        admitted = request.Admitted.AsTask();
                    }
                    else
                    {
                        reply = transport.RequestAsync(stream, payload, operationCancellation.Token).AsTask();
                    }
                    pending.Add(ObserveRequestReplyAsync(
                        reply,
                        streamMetrics,
                        trigger,
                        runId,
                        sequence,
                        started,
                        operationCancellation.Token
                    ));
                    admitted?.GetAwaiter().GetResult();
                    if (++issuedSincePrune == 256)
                    {
                        pending.RemoveAll(static task => task.IsCompleted);
                        issuedSincePrune = 0;
                        Thread.Yield();
                    }
                }
                catch (OperationCanceledException) when (operationCancellation.IsCancellationRequested)
                {
                    streamMetrics.Cancel();
                    throw;
                }
                catch (Exception error)
                {
                    streamMetrics.Complete(started, false, error);
                }
            }
            try
            {
                Task.WhenAll(pending).WaitAsync(TimeSpan.FromMilliseconds(drainBoundMs), operationCancellation.Token)
                    .GetAwaiter().GetResult();
            }
            catch (TimeoutException)
            {
                streamMetrics.FreezeTerminal();
                operationCancellation.Cancel();
            }
            catch (OperationCanceledException) when (operationCancellation.IsCancellationRequested)
            {
                streamMetrics.FreezeTerminal();
            }
        });
        foreach (var streamMetrics in locals)
            metrics.MergeFrom(streamMetrics);
    }

    private static async Task ObserveRequestReplyAsync(
        Task<BenchPayload> replyTask,
        SourceMetrics metrics,
        BenchTriggerRequest trigger,
        uint runId,
        ulong sequence,
        long started,
        CancellationToken cancellationToken
    )
    {
        try
        {
            var reply = await replyTask;
            ValidateReply(reply, runId, metrics.Phase, trigger.payloadBytes, sequence);
            metrics.Complete(started, true);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            metrics.Cancel();
            throw;
        }
        catch (Exception error)
        {
            metrics.Complete(started, false, error);
        }
    }

    private static Task RunSendWorkersAsync(
        IBenchTransport transport,
        SourceMetrics metrics,
        BenchTriggerRequest trigger,
        uint runId,
        Sequence next,
        long deadline,
        int workers,
        CancellationToken cancellationToken
    )
    {
        var locals = new SourceMetrics[workers];
        RunDedicatedStreams(workers, stream =>
        {
            var streamMetrics = locals[stream] = new SourceMetrics(metrics.SampleLimit);
            streamMetrics.Reset(deadline, metrics.Phase);
            while (Stopwatch.GetTimestamp() < deadline)
            {
                var sequence = next.Next();
                var payload = BenchMetricHeaders.CreatePayload(
                    trigger.payloadBytes,
                    runId,
                    streamMetrics.Phase,
                    sequence
                );
                var started = streamMetrics.Begin();
                try
                {
                    transport.SendAsync(stream, payload, cancellationToken)
                        .AsTask()
                        .GetAwaiter()
                        .GetResult();
                    streamMetrics.Complete(started, true);
                }
                catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
                {
                    streamMetrics.Cancel();
                    throw;
                }
                catch (Exception error)
                {
                    streamMetrics.Complete(started, false, error);
                }
            }
        });
        foreach (var streamMetrics in locals)
            metrics.MergeFrom(streamMetrics);
        return Task.CompletedTask;
    }

    private static void RunDedicatedStreams(int count, Action<int> submit)
    {
        var errors = new Exception?[count];
        var threads = Enumerable.Range(0, count).Select(index => new Thread(() =>
        {
            try { submit(index); }
            catch (Exception error) { errors[index] = error; }
        }) { IsBackground = true, Name = $"bench-submit-{index}" }).ToArray();
        foreach (var thread in threads) thread.Start();
        foreach (var thread in threads) thread.Join();
        var failures = errors.Where(static error => error is not null).Cast<Exception>().ToArray();
        if (failures.Length > 0) throw new AggregateException(failures);
    }

    private static async Task ExecuteRequestAsync(
        IBenchTransport transport,
        SourceMetrics metrics,
        BenchTriggerRequest trigger,
        uint runId,
        int stream,
        ulong sequence,
        CancellationToken cancellationToken
    )
    {
        var payload = BenchMetricHeaders.CreatePayload(
            trigger.payloadBytes,
            runId,
            metrics.Phase,
            sequence
        );
        var started = metrics.Begin();
        await ObserveRequestReplyAsync(
            transport.RequestAsync(stream, payload, cancellationToken).AsTask(),
            metrics,
            trigger,
            runId,
            sequence,
            started,
            cancellationToken
        );
    }

    private static void ValidateReply(
        BenchPayload reply,
        uint runId,
        BenchPhase phase,
        int payloadBytes,
        ulong sequence
    )
    {
        BenchPayloads.Validate(reply, payloadBytes);
        if (
            !BenchMetricHeaders.TryDecode(reply, out var header)
            || !BenchMetricHeaders.IsExpected(header, runId, phase, payloadBytes, sequence)
        )
        {
            throw new InvalidOperationException(
                "Echo reply did not carry the expected metric header."
            );
        }
    }

    private static uint HeaderRunId(string runId)
    {
        var hash = 2166136261U;
        foreach (var value in Encoding.UTF8.GetBytes(runId))
        {
            hash = (hash ^ value) * 16777619U;
        }
        return hash == 0 ? 1U : hash;
    }
}
