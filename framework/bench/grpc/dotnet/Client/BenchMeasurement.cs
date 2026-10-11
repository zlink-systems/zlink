using System.Diagnostics;
using System.Net.Http.Json;
using System.Text;
using Systems.Zlink;
using WithGrpcBench.Shared;

namespace WithGrpcBench.Client;

internal static class BenchMeasurement
{
    internal static void ConfigureThreadPool(int sendConcurrency)
    {
        ThreadPool.GetMinThreads(out var workers, out var completionPorts);
        ThreadPool.SetMinThreads(
            Math.Max(workers, sendConcurrency + Environment.ProcessorCount * 2),
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
                if (transport is RawBenchTransport raw)
                {
                    await RunRawRequestBackpressureAsync(
                        raw,
                        metrics,
                        trigger,
                        runId,
                        next,
                        deadline,
                        operationCancellation,
                        options.DrainBoundMs
                    );
                }
                else
                {
                    await RunRequestBackpressureAsync(
                        transport,
                        metrics,
                        trigger,
                        runId,
                        next,
                        deadline,
                        operationCancellation,
                        options.DrainBoundMs
                    );
                }
                break;
            case "send-saturation":
                await RunSendWorkersAsync(
                    transport,
                    metrics,
                    trigger,
                    runId,
                    next,
                    deadline,
                    trigger.sendConcurrency,
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
            snapshot.OtherErrors
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
        var tasks = Enumerable
            .Range(0, workers)
            .Select(_ =>
                Task.Run(
                    async () =>
                    {
                        while (Stopwatch.GetTimestamp() < deadline)
                        {
                            var sequence = next.Next();
                            await ExecuteRequestAsync(
                                transport,
                                metrics,
                                trigger,
                                runId,
                                stream,
                                sequence,
                                cancellationToken
                            );
                        }
                    },
                    cancellationToken
                )
            );
        await transport.DriveAsync(Task.WhenAll(tasks));
    }

    private static async Task RunRequestBackpressureAsync(
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
        var pending = new HashSet<Task>();
        var issuedSincePump = 0;
        while (Stopwatch.GetTimestamp() < deadline)
        {
            var sequence = next.Next();
            pending.Add(
                ExecuteRequestAsync(
                    transport,
                    metrics,
                    trigger,
                    runId,
                    0,
                    sequence,
                    operationCancellation.Token
                )
            );
            if (++issuedSincePump == 256)
            {
                pending.RemoveWhere(static task => task.IsCompleted);
                issuedSincePump = 0;
                await Task.Yield();
            }
        }

        if (pending.Count == 0)
            return;
        try
        {
            await Task.WhenAll(pending)
                .WaitAsync(TimeSpan.FromMilliseconds(drainBoundMs), operationCancellation.Token);
        }
        catch (TimeoutException)
        {
            metrics.FreezeTerminal();
            operationCancellation.Cancel();
            try
            {
                await Task.WhenAll(pending).WaitAsync(TimeSpan.FromSeconds(5));
            }
            catch (OperationCanceledException) when (operationCancellation.IsCancellationRequested)
            {
                // The drain-bound cancellation is already represented by abandoned.
            }
            catch (TimeoutException)
            {
                // The recorded abandoned count is the bounded observation result.
            }
        }
    }

    private static async Task RunRawRequestBackpressureAsync(
        RawBenchTransport transport,
        SourceMetrics metrics,
        BenchTriggerRequest trigger,
        uint runId,
        Sequence next,
        long deadline,
        CancellationTokenSource operationCancellation,
        int drainBoundMs
    )
    {
        var pendingReplies = new HashSet<Task>();
        Task? pendingAdmission = null;
        Task[] pending;

        while (Stopwatch.GetTimestamp() < deadline)
        {
            var submitted = false;
            if (pendingAdmission is null || pendingAdmission.IsCompleted)
            {
                pendingAdmission = null;
                var sequence = next.Next();
                var payload = BenchMetricHeaders.CreatePayload(
                    trigger.payloadBytes,
                    runId,
                    metrics.Phase,
                    sequence
                );
                var started = metrics.Begin();
                try
                {
                    RawRequestSubmission submission = transport.SubmitRequest(
                        payload,
                        operationCancellation.Token
                    );
                    pendingReplies.Add(
                        ObserveRawRequestAsync(
                            submission.Reply,
                            metrics,
                            trigger,
                            runId,
                            sequence,
                            started,
                            operationCancellation.Token
                        )
                    );
                    submitted = true;
                    if (submission.Result == SubmitResult.Backpressured)
                    {
                        // The reply observer is already running. Only this raw
                        // socket waits for its writable admission completion.
                        pendingAdmission = ObserveRawAdmissionAsync(
                            submission.Admitted,
                            operationCancellation.Token
                        );
                    }
                    else if (submission.Result != SubmitResult.Ok)
                    {
                        throw new InvalidOperationException(
                            $"Raw request submit returned {submission.Result}."
                        );
                    }
                }
                catch (OperationCanceledException)
                    when (operationCancellation.IsCancellationRequested)
                {
                    metrics.Cancel();
                    throw;
                }
                catch (Exception error)
                {
                    metrics.Complete(started, false, error);
                }
            }

            pendingReplies.RemoveWhere(static task => task.IsCompleted);
            var waitMs = submitted ? 0 : RemainingPollTimeoutMs(deadline);
            transport.WaitRequestCompletion(waitMs);
        }

        pending = pendingAdmission is null
            ? pendingReplies.ToArray()
            : pendingReplies.Append(pendingAdmission).ToArray();
        if (pending.Length == 0)
            return;
        var allPending = Task.WhenAll(pending);
        var drainDeadline =
            Stopwatch.GetTimestamp()
            + checked((long)(Stopwatch.Frequency * (drainBoundMs / 1000.0)));
        while (
            !allPending.IsCompleted
            && Stopwatch.GetTimestamp() < drainDeadline
            && !operationCancellation.IsCancellationRequested
        )
            transport.WaitRequestCompletion(RemainingPollTimeoutMs(drainDeadline));
        try
        {
            await allPending.WaitAsync(TimeSpan.Zero, operationCancellation.Token);
        }
        catch (TimeoutException)
        {
            metrics.FreezeTerminal();
            operationCancellation.Cancel();
            try
            {
                var cancellationDeadline = Stopwatch.GetTimestamp() + 5 * Stopwatch.Frequency;
                while (!allPending.IsCompleted && Stopwatch.GetTimestamp() < cancellationDeadline)
                    transport.WaitRequestCompletion(RemainingPollTimeoutMs(cancellationDeadline));
                await allPending.WaitAsync(TimeSpan.Zero);
            }
            catch (OperationCanceledException) when (operationCancellation.IsCancellationRequested)
            {
                // The drain-bound cancellation is already represented by abandoned.
            }
            catch (TimeoutException)
            {
                // The recorded abandoned count is the bounded observation result.
            }
        }
    }

    private static async Task ObserveRawRequestAsync(
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

    private static async Task ObserveRawAdmissionAsync(
        Task admission,
        CancellationToken cancellationToken
    )
    {
        try
        {
            await admission;
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            // Reply observation accounts for the cancelled request.
        }
        catch
        {
            // Reply observation accounts for the failed request.
        }
    }

    private static int RemainingPollTimeoutMs(long deadline)
    {
        var remainingTicks = deadline - Stopwatch.GetTimestamp();
        if (remainingTicks <= 0)
            return 0;
        var remainingMs = remainingTicks * 1000.0 / Stopwatch.Frequency;
        return Math.Min(
            BenchPolling.CompletionPollIntervalMs,
            Math.Max(1, (int)Math.Ceiling(remainingMs))
        );
    }

    private static async Task RunSendWorkersAsync(
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
        var tasks = Enumerable
            .Range(0, workers)
            .Select(stream =>
                Task.Run(
                    async () =>
                    {
                        while (Stopwatch.GetTimestamp() < deadline)
                        {
                            var sequence = next.Next();
                            var payload = BenchMetricHeaders.CreatePayload(
                                trigger.payloadBytes,
                                runId,
                                metrics.Phase,
                                sequence
                            );
                            var started = metrics.Begin();
                            try
                            {
                                await transport.SendAsync(stream, payload, cancellationToken);
                                metrics.Complete(started, true);
                            }
                            catch (OperationCanceledException)
                                when (cancellationToken.IsCancellationRequested)
                            {
                                metrics.Cancel();
                                throw;
                            }
                            catch (Exception error)
                            {
                                metrics.Complete(started, false, error);
                            }
                        }
                    },
                    cancellationToken
                )
            );
        await transport.DriveAsync(Task.WhenAll(tasks));
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
        try
        {
            var reply = await transport.RequestAsync(stream, payload, cancellationToken);
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
