using System.Globalization;
using System.Text.Json;
using Xunit;

namespace ZLink.Framework.Perf.Tests;

public sealed class HarnessContractTests
{
    private static RoleConfig Config(double seconds = .05) =>
        new(
            "test",
            "session-echo-only/1024/test",
            new string('a', 64),
            "client",
            0,
            "session-echo-only",
            null,
            null,
            null,
            [],
            null,
            "",
            "",
            false,
            "None",
            null,
            [],
            [],
            "Immediate",
            TestWorkload.Create(seconds, seconds, connections: 1, logicalStreams: null),
            []
        );

    private static PerfTriggerRequest Trigger(Measurement measurement, string phase, string seq) =>
        new()
        {
            runId = measurement.Config.runId,
            cellId = measurement.Config.cellId,
            phase = phase,
            resetSeq = seq,
        };

    private static ResetRequest Reset(Measurement measurement, string seq = "1") =>
        new()
        {
            runId = measurement.Config.runId,
            cellId = measurement.Config.cellId,
            resetSeq = seq,
        };

    [Fact]
    public void RunnerWorkloadConfigReadsDrainDeadlineWithoutLegacyTimeouts()
    {
        var workload = PerfJson.Read<Workload>(
            """
            {"payloadSize":1024,"durationSeconds":2,"warmupSeconds":1,"connections":null,
             "logicalStreams":10,"clientCount":1,"connectConcurrency":null,"drainTimeoutMs":30000,
             "setupTimeoutMs":30000,"adminTimeoutMs":5000,"socketSendTimeoutMs":33000}
            """
        );
        var worker = PerfJson.Read<WorkerConfig>(
            """
            {"algorithm":"xorshift32-v1","taskMillis":5,"minThreads":8,"maxThreads":8,"idleTimeoutMs":60000}
            """
        );

        Assert.Equal(30_000, workload.drainTimeoutMs);
        Assert.Equal(60_000, worker.idleTimeoutMs);
    }

    [Fact]
    public async Task RequestStreamsSubmitWithoutWaitingForRepliesAndDrainBeforePhaseCompletes()
    {
        var config = Config(.02) with
        {
            workload = TestWorkload.Create(.02, .02, drainTimeoutMs: 70, setupTimeoutMs: 90),
        };
        using var measurement = new Measurement(config, true);
        var twoPending = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var issued = 0;

        async Task Operation(int stream)
        {
            if (!measurement.BeginOperation(out var started))
                return;
            if (Interlocked.Increment(ref issued) <= 2)
            {
                if (Volatile.Read(ref issued) == 2)
                    twoPending.TrySetResult();
                await release.Task;
            }
            measurement.CompleteOperation(started);
        }

        Assert.True(
            measurement
                .Start(
                    Trigger(measurement, "warmup", "0"),
                    () => ServerDrivenStreams.RunRequestsAsync(measurement, 1, Operation)
                )
                .accepted
        );
        await twoPending.Task.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.False(measurement.PhaseTask.IsCompleted);
        release.TrySetResult();
        await measurement.PhaseTask.WaitAsync(TimeSpan.FromSeconds(2));

        Assert.True(issued >= 2);
    }

    [Fact]
    public async Task CallDeadlineUsesSetupTimeoutThenPhaseEndPlusDrain()
    {
        var config = Config(.01) with
        {
            workload = TestWorkload.Create(.01, .01, drainTimeoutMs: 70, setupTimeoutMs: 90),
        };
        using var measurement = new Measurement(config, true);
        var setupDeadline = measurement.CallDeadlineTicks();
        Assert.InRange(setupDeadline - PerfClock.Now, 1L, 90_000_000L);

        Assert.True(measurement.Start(Trigger(measurement, "warmup", "0"), null).accepted);
        Assert.Equal(measurement.EndTicks + 70_000_000L, measurement.CallDeadlineTicks());
        await measurement.PhaseTask.WaitAsync(TimeSpan.FromSeconds(2));
    }

    [Theory]
    [InlineData(false, true)]
    [InlineData(true, true)]
    [InlineData(true, false)]
    public async Task WarmupWaitsForOperationsAndHandlersBeforeCompleting(
        bool pendingOperation,
        bool handlerLast
    )
    {
        using var measurement = new Measurement(Config(), true);
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        long started = 0;
        var handlerActive = false;
        var operationActive = false;
        Assert.True(
            measurement
                .Start(
                    Trigger(measurement, "warmup", "0"),
                    () =>
                    {
                        measurement.HandlerEnter();
                        handlerActive = true;
                        if (pendingOperation)
                        {
                            Assert.True(measurement.BeginOperation(out started));
                            operationActive = true;
                        }
                        entered.SetResult();
                        return Task.CompletedTask;
                    }
                )
                .accepted
        );
        await entered.Task.WaitAsync(TimeSpan.FromSeconds(2));
        try
        {
            await Task.Delay(80);
            if (pendingOperation)
            {
                if (handlerLast)
                {
                    measurement.CompleteOperation(started);
                    operationActive = false;
                }
                else
                {
                    measurement.HandlerExit();
                    handlerActive = false;
                }
            }
            var reset = measurement.Reset(Reset(measurement), null);
            Assert.False(
                measurement.PhaseTask.IsCompleted,
                $"Warmup completed with unfinished work; reset ok={reset.ok}, reason={reset.reason}"
            );
            Assert.False(measurement.WaitForOperationsAsync().IsCompleted);
            Assert.False(reset.ok);
        }
        finally
        {
            if (operationActive)
                measurement.CompleteOperation(started);
            if (handlerActive)
                measurement.HandlerExit();
            await measurement.PhaseTask.WaitAsync(TimeSpan.FromSeconds(2));
        }
        Assert.True(measurement.Reset(Reset(measurement), null).ok);
    }

    [Fact]
    public async Task MeasuredPhaseSealsAtWindowEndWithAnOutstandingOperation()
    {
        var config = Config(.01) with
        {
            workload = TestWorkload.Create(.01, .01, drainTimeoutMs: 200, setupTimeoutMs: 90),
        };
        using var measurement = new Measurement(config, true);
        Assert.True(measurement.Start(Trigger(measurement, "warmup", "0"), null).accepted);
        await measurement.PhaseTask.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.True(measurement.Reset(Reset(measurement), null).ok);

        var issued = new TaskCompletionSource<long>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var producerFinished = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        Assert.True(
            measurement
                .Start(
                    Trigger(measurement, "measured", "1"),
                    async () =>
                    {
                        Assert.True(measurement.BeginOperation(out var started));
                        issued.TrySetResult(started);
                        await release.Task;
                        measurement.CompleteOperation(started);
                        producerFinished.TrySetResult();
                    }
                )
                .accepted
        );
        var operationStarted = await issued.Task.WaitAsync(TimeSpan.FromSeconds(2));
        try
        {
            await measurement.PhaseTask.WaitAsync(TimeSpan.FromSeconds(2));
            var snapshot = measurement.Snapshot(null);
            Assert.Equal("complete", snapshot.phase);
            Assert.Equal("1", snapshot.metrics["messages.sent"]);
            Assert.Equal("0", snapshot.metrics["messages.completed"]);
            Assert.Equal("1", snapshot.metrics["messages.inflightAtEnd"]);
            Assert.False(measurement.CompleteOperation(operationStarted));
        }
        finally
        {
            release.TrySetResult();
            await producerFinished.Task.WaitAsync(TimeSpan.FromSeconds(2));
        }
    }

    [Fact]
    public async Task CompleteOperationReturnsWhetherSuccessWasInsideTheWindow()
    {
        using var measurement = new Measurement(Config(), true);
        Func<long, Exception?, long?, bool> complete = measurement.CompleteOperation;
        Assert.Equal("0", measurement.Snapshot(null).metrics["messages.inflightAtEnd"]);
        Assert.True(
            measurement
                .Start(
                    Trigger(measurement, "warmup", "0"),
                    () =>
                    {
                        Assert.True(measurement.BeginOperation(out var first));
                        Assert.True(complete(first, null, measurement.EndTicks - 1));
                        Assert.True(measurement.BeginOperation(out var second));
                        Assert.False(complete(second, null, measurement.EndTicks));
                        return Task.CompletedTask;
                    }
                )
                .accepted
        );
        await measurement.PhaseTask;
        var snapshot = measurement.Snapshot(null);
        Assert.Equal("1", snapshot.metrics["messages.completed"]);
        Assert.Equal("1", snapshot.metrics["messages.inflightAtEnd"]);
    }

    [Fact]
    public void PayloadValidatesEveryByteAndCanonicalPaddedBase64()
    {
        foreach (var size in new[] { 1024, 4096 })
        {
            var pattern = new PayloadPattern(size);
            pattern.Validate(pattern.Base64);
            var bytes = Convert.FromBase64String(pattern.Base64);
            Assert.Equal(size, bytes.Length);
            Assert.Equal(29, bytes[0]);
            bytes[size - 1] ^= 1;
            Assert.Throws<PerfValidationException>(() =>
                pattern.Validate(Convert.ToBase64String(bytes))
            );
            Assert.Throws<PerfValidationException>(() => pattern.Validate(pattern.Base64 + "\n"));
        }
    }

    [Fact]
    public void HistogramKeepsExactSumOverflowAndInclusiveBounds()
    {
        var bounds = Histogram.Bounds;
        var firstBoundaryNs = checked((long)(bounds[0] * 1_000_000));
        var firstAboveNs = firstBoundaryNs + 1;
        var overflowNs = checked((long)(bounds[^1] * 1_000_000) + 1);
        var histogram = new Histogram();
        histogram.Record(firstBoundaryNs);
        histogram.Record(firstAboveNs);
        histogram.Record(overflowNs);
        Dictionary<string, object?> metrics = [],
            histograms = [];
        Dictionary<string, NullReason> reasons = [];
        histogram.Export("latencyMs", "latency", metrics, histograms, reasons);
        var snapshot = histogram.Snapshot();
        Assert.Equal(bounds, snapshot.bounds);
        Assert.Equal("1", snapshot.counts[0]);
        Assert.Equal("1", snapshot.counts[1]);
        Assert.Equal("1", snapshot.overflow);
        Assert.Equal("3", snapshot.count);
        Assert.Equal(
            (firstBoundaryNs + firstAboveNs + overflowNs).ToString(CultureInfo.InvariantCulture),
            snapshot.sumNs
        );
        Assert.Equal(bounds[1], metrics["latency.p50Ms"]);
        Assert.Null(metrics["latency.p95Ms"]);
        Assert.Equal("nearest-rank-bucket-upper-bound-capped-by-max", snapshot.percentileMethod);
        Assert.Equal("HISTOGRAM_OVERFLOW", reasons["/metrics/latency.p95Ms"].code);
        Assert.Equal(snapshot.bounds[^1], reasons["/metrics/latency.p95Ms"].lowerBoundMs);
        Assert.Equal(overflowNs / 1_000_000d, metrics["latency.maxMs"]);
    }

    [Fact]
    public void HistogramCapsRegularBucketPercentileAtObservedMaximum()
    {
        var sampleNs = checked((long)(Histogram.Bounds[0] * 1_000_000) + 1);
        var histogram = new Histogram();
        histogram.Record(sampleNs);
        Dictionary<string, object?> metrics = [],
            histograms = [];
        Dictionary<string, NullReason> reasons = [];
        histogram.Export("latencyMs", "latency", metrics, histograms, reasons);
        Assert.Equal(sampleNs / 1_000_000d, metrics["latency.p50Ms"]);
        Assert.Equal(sampleNs / 1_000_000d, metrics["latency.p95Ms"]);
        Assert.DoesNotContain("/metrics/latency.p95Ms", reasons.Keys);
    }

    [Fact]
    public void EmptyHistogramHasReasonsForEveryLatencyAndMax()
    {
        Dictionary<string, object?> metrics = [],
            histograms = [];
        Dictionary<string, NullReason> reasons = [];
        new Histogram().Export("latencyMs", "latency", metrics, histograms, reasons);
        Assert.All(metrics.Values, Assert.Null);
        Assert.Equal(6, reasons.Count);
        Assert.All(reasons.Values, value => Assert.Equal("NO_SAMPLES", value.code));
    }

    [Theory]
    [InlineData("01")]
    [InlineData("+1")]
    [InlineData("-0")]
    [InlineData("18446744073709551616")]
    public void DecimalU64RejectsNoncanonicalOrOutOfRange(string value) =>
        Assert.Throws<JsonException>(() => DecimalText.U64(value));

    [Fact]
    public void JsonPreservesAll64BitsAndRejectsNumberTokens()
    {
        var json = PerfJson.Write(new { unsigned = ulong.MaxValue, signed = long.MinValue });
        Assert.Contains("\"18446744073709551615\"", json);
        Assert.Contains("\"-9223372036854775808\"", json);
        Assert.Equal(ulong.MaxValue, PerfJson.Read<ulong>("\"18446744073709551615\""));
        Assert.Throws<JsonException>(() => PerfJson.Read<ulong>("18446744073709551615"));
        Assert.Throws<JsonException>(() =>
            PerfJson.Read<ResetRequest>("{\"runId\":\"a\",\"cellId\":\"b\"}")
        );
        Assert.Throws<JsonException>(() =>
            PerfJson.Read<ResetRequest>("{\"runId\":\"a\",\"cellId\":\"b\",\"resetSeq\":1}")
        );
    }

    [Fact]
    public async Task ResetRejectsAnOutstandingWarmupAndMeasuredCannotStartBeforeReset()
    {
        using var measurement = new Measurement(Config(), true);
        var outstanding = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        Assert.True(
            measurement
                .Start(
                    Trigger(measurement, "warmup", "0"),
                    async () =>
                    {
                        Assert.True(measurement.BeginOperation(out var started));
                        await outstanding.Task;
                        measurement.CompleteOperation(started);
                    }
                )
                .accepted
        );
        await Task.Delay(80); // Test controls a pending operation beyond the warmup window.
        Assert.False(measurement.Reset(Reset(measurement), null).ok);
        Assert.False(measurement.Start(Trigger(measurement, "measured", "1"), null).accepted);
        outstanding.SetResult();
        await measurement.PhaseTask;
        var ack = measurement.Reset(Reset(measurement), null);
        Assert.True(ack.ok);
        Assert.Same(ack, measurement.Reset(Reset(measurement), null));
        Assert.False(measurement.Reset(Reset(measurement, "0"), null).ok);
        Assert.True(measurement.Start(Trigger(measurement, "measured", "1"), null).accepted);
        Assert.False(measurement.Reset(Reset(measurement), null).ok);
        await measurement.PhaseTask;
        Assert.Same(ack, measurement.Reset(Reset(measurement), null));
    }

    [Fact]
    public async Task PublicStatusSamplingDoesNotHoldTheApplicationCounterLock()
    {
        using var measurement = new Measurement(Config(.12), true);
        measurement.SamplePublicState = () =>
        {
            var read = Task.Run(() => measurement.Phase);
            Assert.True(
                read.Wait(TimeSpan.FromSeconds(1)),
                "Public status observation held the application counter lock."
            );
            return new { phase = read.Result };
        };
        Assert.True(measurement.Start(Trigger(measurement, "warmup", "0"), null).accepted);
        await measurement.PhaseTask;
    }

    [Fact]
    public async Task OperationsStillPendingAtWindowEndAreCountedAsInflightAtEnd()
    {
        using var measurement = new Measurement(Config(.08), true);
        measurement.Start(Trigger(measurement, "warmup", "0"), null);
        await measurement.PhaseTask;
        Assert.True(measurement.Reset(Reset(measurement), null).ok);
        var pending = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var calls = 0;
        Func<Task> workload = async () =>
        {
            Interlocked.Increment(ref calls);
            Assert.True(measurement.BeginOperation(out var started));
            entered.SetResult();
            await pending.Task;
            measurement.CompleteOperation(started);
        };
        var trigger = Trigger(measurement, "measured", "1");
        Assert.True(measurement.Start(trigger, workload).accepted);
        Assert.Equal("alreadyStarted", measurement.Start(trigger, workload).state);
        await entered.Task;
        await Task.Delay(100);
        Assert.False(measurement.BeginOperation(out _));
        pending.SetResult();
        await measurement.PhaseTask;
        var snapshot = measurement.Snapshot(null);
        Assert.Equal(1, calls);
        Assert.Equal("1", snapshot.metrics["messages.sent"]);
        Assert.Equal("0", snapshot.metrics["messages.completed"]);
        Assert.Equal("0", snapshot.metrics["messages.failed"]);
        Assert.Equal("0", snapshot.metrics["messages.timeout"]);
        Assert.Equal("0", snapshot.metrics["messages.cancelled"]);
        Assert.Equal("1", snapshot.metrics["messages.inflightAtEnd"]);
        Assert.Equal(
            "1",
            Sum(
                "messages.completed",
                "messages.failed",
                "messages.timeout",
                "messages.cancelled",
                "messages.inflightAtEnd"
            )
        );
        Assert.DoesNotContain("messages.settleCompleted", snapshot.metrics.Keys);
        Assert.DoesNotContain("settleLatencyMs", snapshot.histograms.Keys);
        Assert.Equal(0.0, snapshot.metrics["throughput.kops"]);
        Assert.Equal("0", Assert.IsType<HistogramSnapshot>(snapshot.histograms["latencyMs"]).count);

        string Sum(params string[] keys) =>
            keys.Aggregate(0UL, (total, key) => total + ulong.Parse((string)snapshot.metrics[key]!))
                .ToString();
    }

    [Fact]
    public async Task PublicOperationCanceledExceptionIsClassifiedAsCancelled()
    {
        using var measurement = new Measurement(Config(), true);
        Assert.True(
            measurement
                .Start(
                    Trigger(measurement, "warmup", "0"),
                    () =>
                    {
                        Assert.True(measurement.BeginOperation(out var started));
                        measurement.CompleteOperation(
                            started,
                            new OperationCanceledException("test cancellation")
                        );
                        return Task.CompletedTask;
                    }
                )
                .accepted
        );
        await measurement.PhaseTask;
        var snapshot = measurement.Snapshot(null);
        Assert.Equal("1", snapshot.metrics["messages.cancelled"]);
        Assert.Equal("0", snapshot.metrics["messages.failed"]);
        Assert.Equal("0", snapshot.metrics["messages.timeout"]);
    }
}
