using System.Collections.Concurrent;
using System.Text.Json;

namespace Zlink.Framework.UnitTests;

public sealed class RuntimeConformanceFixtureTests
{
    [ThreadStatic]
    private static bool _insideAdmissionCall;

    [Fact]
    public void Serial_execution_arbitration_limits_match_the_shared_fixture()
    {
        using var document = Load("serial-execution-v1.json");
        var limits = document.RootElement.GetProperty("limits");

        Assert.Equal(
            ZLinkSerialExecutionQueue.OwnerTimeSliceMilliseconds,
            limits.GetProperty("ownerTimeBudgetMilliseconds").GetInt32());
        Assert.Equal(
            ZLinkSerialExecutionQueue.LifecycleTurnLimit,
            limits.GetProperty("lifecycleBurstLimit").GetInt32());
    }

    [Fact]
    public void Serial_work_fifo_preserves_order_and_detaches_items_for_reappend()
    {
        var first = new ZLinkSerialWorkItem(
            static _ => ValueTask.CompletedTask);
        var second = new ZLinkSerialWorkItem(
            static _ => ValueTask.CompletedTask);
        var third = new ZLinkSerialWorkItem(
            static _ => ValueTask.CompletedTask);
        var source = new ZLinkSerialWorkQueue();
        var target = new ZLinkSerialWorkQueue();

        source.Enqueue(first);
        source.Enqueue(second);
        source.Enqueue(third);

        Assert.Equal(3, source.Count);
        Assert.Equal([first, second, third], source.ToArray());
        Assert.Throws<InvalidOperationException>(() => target.Enqueue(second));

        Assert.True(source.TryDequeue(out var dequeued));
        Assert.Same(first, dequeued);
        Assert.Null(first.Next);
        target.Enqueue(first);

        source.Clear();
        Assert.Equal(0, source.Count);
        Assert.Null(second.Next);
        Assert.Null(third.Next);
        target.Enqueue(second);
        target.Enqueue(third);

        Assert.Equal([first, second, third], target.ToArray());
        target.Clear();
        Assert.Null(first.Next);
        Assert.Null(second.Next);
        Assert.Null(third.Next);

        source.Enqueue(third);
        Assert.True(source.TryDequeue(out var reappended));
        Assert.Same(third, reappended);
        Assert.False(source.TryDequeue(out _));
    }

    [Fact]
    public void Serial_work_fifo_append_allocates_no_storage()
    {
        var warmup = new ZLinkSerialWorkQueue();
        var warmupItem = new ZLinkSerialWorkItem(
            static _ => ValueTask.CompletedTask);
        warmup.Enqueue(warmupItem);
        Assert.True(warmup.TryDequeue(out _));

        var queue = new ZLinkSerialWorkQueue();
        var first = new ZLinkSerialWorkItem(
            static _ => ValueTask.CompletedTask);
        var second = new ZLinkSerialWorkItem(
            static _ => ValueTask.CompletedTask);
        var third = new ZLinkSerialWorkItem(
            static _ => ValueTask.CompletedTask);
        var before = GC.GetAllocatedBytesForCurrentThread();

        queue.Enqueue(first);
        queue.Enqueue(second);
        queue.Enqueue(third);

        var after = GC.GetAllocatedBytesForCurrentThread();
        Assert.Equal(before, after);
    }

    [Fact]
    public async Task Serial_count_admission_is_independent_per_physical_lane()
    {
        using var document = Load("serial-execution-v1.json");
        var scenarios = document.RootElement.GetProperty("accountingScenarios");
        var applicationCount = Scenario(scenarios, "application-count-boundary")
            .GetProperty("acceptedWorkCount").GetInt32();
        var lifecycleCount = Scenario(scenarios, "lifecycle-count-boundary")
            .GetProperty("acceptedWorkCount").GetInt32();
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var queue = CreateQueue(errorSink);
        var firstStarted = Signal();
        var releaseFirst = Signal();

        Assert.Equal(
            ZLinkSerialPostAdmission.Accepted,
            queue.TryPostApplicationWithAdmission(
                async _ =>
                {
                    firstStarted.TrySetResult();
                    await releaseFirst.Task.ConfigureAwait(false);
                },
                out _));
        await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        for (var index = 1; index < applicationCount; index++)
            Assert.Equal(
                ZLinkSerialPostAdmission.Accepted,
                queue.TryPostApplicationWithAdmission(
                    static _ => ValueTask.CompletedTask,
                    out _));
        Assert.Equal(applicationCount, queue.ApplicationPendingCount);
        Assert.Equal(
            ZLinkSerialPostAdmission.Accepted,
            queue.TryPostApplicationWithAdmission(
                static _ => ValueTask.CompletedTask,
                out _));

        ZLinkSerialWorkItem? lastLifecycle = null;
        for (var index = 0; index < lifecycleCount; index++)
        {
            Assert.Equal(
                ZLinkSerialPostAdmission.Accepted,
                queue.TryPostNextWithAdmission(
                    static _ => ValueTask.CompletedTask,
                    out var accepted));
            lastLifecycle = accepted;
        }
        Assert.Equal(lifecycleCount, queue.LifecyclePendingCount);
        Assert.Equal(
            ZLinkSerialPostAdmission.Accepted,
            queue.TryPostNextWithAdmission(
                static _ => ValueTask.CompletedTask,
                out _));

        releaseFirst.TrySetResult();
        await queue.ApplicationDrained.WaitAsync(TimeSpan.FromSeconds(10));
        await lastLifecycle!.Completion.WaitAsync(TimeSpan.FromSeconds(10));
    }

    [Fact]
    public async Task Serial_byte_admission_counts_fixed_and_retained_bytes_per_lane()
    {
        using var document = Load("serial-execution-v1.json");
        var scenarios = document.RootElement.GetProperty("accountingScenarios");
        var applicationBytes = Scenario(scenarios, "application-byte-boundary")
            .GetProperty("retainedPayloadBytesPerWork").GetInt64();
        var lifecycleBytes = Scenario(scenarios, "lifecycle-byte-boundary")
            .GetProperty("retainedPayloadBytesPerWork").GetInt64();
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var queue = CreateQueue(errorSink);
        var releaseApplication = Signal();
        var releaseLifecycle = Signal();
        try
        {
            Assert.Equal(
                ZLinkSerialPostAdmission.Accepted,
                queue.TryPostApplicationWithAdmission(
                    async _ => await releaseApplication.Task.ConfigureAwait(false),
                    out var application));
            Assert.Equal(
                ZLinkSerialPostAdmission.Accepted,
                queue.TryPostApplicationWithAdmission(
                    static _ => ValueTask.CompletedTask,
                    out _));

            Assert.Equal(
                ZLinkSerialPostAdmission.Accepted,
                queue.TryPostNextWithAdmission(
                    async _ => await releaseLifecycle.Task.ConfigureAwait(false),
                    out var lifecycle));
            Assert.Equal(
                ZLinkSerialPostAdmission.Accepted,
                queue.TryPostNextWithAdmission(
                    static _ => ValueTask.CompletedTask,
                    out _));

            releaseApplication.TrySetResult();
            releaseLifecycle.TrySetResult();
            await Task.WhenAll(application.Completion, lifecycle.Completion)
                .WaitAsync(TimeSpan.FromSeconds(5));
        }
        finally
        {
            // A failed assertion must not strand DisposeAsync behind either
            // intentionally blocked callback and mask the actual failure.
            releaseApplication.TrySetResult();
            releaseLifecycle.TrySetResult();
        }
    }

    [Fact]
    public async Task Accepted_work_preserves_sequence_and_post_release_progress()
    {
        using var document = Load("serial-execution-v1.json");
        Assert.True(document.RootElement
            .GetProperty("admissionInvariants")
            .GetProperty("enqueueFailureRestoresReservation")
            .GetBoolean());
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errorSink, CancellationToken.None),
            errorSink,
            CancellationToken.None);
        var release = Signal();
        try
        {
            Assert.Equal(
                ZLinkAcceptedWorkAdmission.Accepted,
                queue.TryPostAccepted(
                    new byte[5],
                    async _ => await release.Task.ConfigureAwait(false),
                    static () => { },
                    out var first));
            Assert.Equal(1UL, first.AcceptedSequence);
            Assert.Equal(1, queue.ApplicationPendingCount);
            Assert.Equal(
                ZLinkAcceptedWorkAdmission.Accepted,
                queue.TryPostAccepted(
                    new byte[13],
                    static _ => ValueTask.CompletedTask,
                    static () => { },
                    out _));
            Assert.Equal(2, queue.ApplicationPendingCount);

            release.TrySetResult();
            await first.Completion.WaitAsync(TimeSpan.FromSeconds(5));
            await queue.ApplicationDrained.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(0, queue.ApplicationPendingCount);
            Assert.Equal(
                ZLinkAcceptedWorkAdmission.Accepted,
                queue.TryPostAccepted(
                    new byte[7],
                    static _ => ValueTask.CompletedTask,
                    static () => { },
                    out var second));
            Assert.Equal(3UL, second.AcceptedSequence);
            Assert.Equal(1, queue.ApplicationPendingCount);
            await second.Completion.WaitAsync(TimeSpan.FromSeconds(5));
        }
        finally
        {
            release.TrySetResult();
        }
    }

    [Fact]
    public async Task Lifecycle_debt_selection_matches_the_shared_fixture()
    {
        using var document = Load("serial-execution-v1.json");
        var scenario = document.RootElement.GetProperty("arbitrationScenarios")[0];
        var expected = scenario.GetProperty("expectedSelection")
            .EnumerateArray().Select(static item => item.GetString()!).ToArray();
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var queue = CreateQueue(errorSink);
        var blockerStarted = Signal();
        var releaseBlocker = Signal();
        var selected = new ConcurrentQueue<string>();

        Assert.True(queue.TryPost(
            async _ =>
            {
                blockerStarted.TrySetResult();
                await releaseBlocker.Task.ConfigureAwait(false);
            },
            out _));
        await blockerStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var completions = new List<Task>();
        foreach (var name in scenario.GetProperty("lifecycleInput")
                     .EnumerateArray().Select(static item => item.GetString()!))
        {
            Assert.True(queue.TryPostNext(
                _ =>
                {
                    selected.Enqueue(name);
                    return ValueTask.CompletedTask;
                },
                out var item));
            completions.Add(item.Completion);
        }
        foreach (var name in scenario.GetProperty("applicationInput")
                     .EnumerateArray().Select(static item => item.GetString()!))
        {
            Assert.True(queue.TryPostApplication(
                _ =>
                {
                    selected.Enqueue(name);
                    return ValueTask.CompletedTask;
                },
                out var item));
            completions.Add(item.Completion);
        }

        releaseBlocker.TrySetResult();
        await Task.WhenAll(completions).WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(expected, selected.ToArray());
    }

    [Fact]
    public async Task Rejected_task_runner_uses_shared_dispatch_without_inline_execution()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        var runner = new ZLinkRuntimeTaskRunner(errorSink, CancellationToken.None);
        await runner.StopAsync();
        await using var queue = new ZLinkSerialExecutionQueue(
            runner,
            errorSink,
            CancellationToken.None);
        var completed = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);

        _insideAdmissionCall = true;
        Assert.True(queue.TryPost(
            _ =>
            {
                completed.TrySetResult(_insideAdmissionCall);
                return ValueTask.CompletedTask;
            },
            out _));
        _insideAdmissionCall = false;

        Assert.False(await completed.Task.WaitAsync(TimeSpan.FromSeconds(5)));
    }

    [Fact]
    public void Same_owner_call_results_match_the_shared_fixture()
    {
        using var document = Load("serial-execution-v1.json");
        using var scope = ZLinkApplicationExecutionContext.Push(
            new ZLinkApplicationExecutionScope(
                "spot-A",
                ZLinkUserSpotExecutionMode.SpotWide,
                "actor-A",
                YieldAllowed: true,
                IsMemberActor: static candidate => candidate == "actor-B"));

        foreach (var scenario in document.RootElement
                     .GetProperty("sameOwnerCalls").EnumerateArray())
        {
            var target = scenario.GetProperty("target").GetString()!;
            Assert.Equal(
                scenario.GetProperty("async").GetString(),
                ObserveNestedResult(target, ZLinkNestedRequestTerminator.Async));
            Assert.Equal(
                scenario.GetProperty("yield").GetString(),
                ObserveNestedResult(target, ZLinkNestedRequestTerminator.Yield));
        }
    }

    [Fact]
    public async Task Runtime_observation_retention_and_loss_match_the_shared_fixture()
    {
        using var document = Load("runtime-observation-v1.json");
        var root = document.RootElement;
        var limits = root.GetProperty("limits");
        Assert.Equal(
            ZLinkObservationQueue<FixtureStatus>.DefaultTerminalCapacity,
            limits.GetProperty("defaultTerminalCapacity").GetInt32());
        Assert.Equal(
            ulong.Parse(limits.GetProperty("signedLossCounterMaximum").GetString()!),
            ZLinkObservationQueue<FixtureStatus>.IncrementLossCounter(ulong.MaxValue));

        var scenario = root.GetProperty("scenarios")[0];
        var queue = new ZLinkObservationQueue<FixtureStatus>(
            static status => status.Source,
            scenario.GetProperty("terminalCapacity").GetInt32());
        foreach (var operation in scenario.GetProperty("operations").EnumerateArray())
        {
            queue.Publish(
                new FixtureStatus(
                    operation.GetProperty("source").GetString()!,
                    operation.GetProperty("sequence").GetUInt64(),
                    operation.GetProperty("value").GetString()!),
                operation.GetProperty("kind").GetString() == "terminal");
        }
        queue.Complete();

        var observed = new List<ZLinkObservedStatus<FixtureStatus>>();
        await foreach (var status in queue.ReadAllAsync())
            observed.Add(status);

        var expectedIntermediate = scenario
            .GetProperty("expectedRetainedIntermediateBySource")
            .EnumerateObject()
            .ToDictionary(
                static item => item.Name,
                static item => item.Value.GetProperty("value").GetString()!,
                StringComparer.Ordinal);
        var expectedTerminal = scenario.GetProperty("expectedTerminalFifo")
            .EnumerateArray().Select(static item => item.GetProperty("value").GetString()!)
            .ToArray();
        Assert.Equal(
            expectedIntermediate,
            observed
                .Where(item => expectedIntermediate.ContainsKey(item.Status.Source))
                .ToDictionary(
                    static item => item.Status.Source,
                    static item => item.Status.Value,
                    StringComparer.Ordinal));
        Assert.Equal(
            expectedTerminal,
            observed
                .Where(item => !expectedIntermediate.ContainsKey(item.Status.Source))
                .Select(static item => item.Status.Value)
                .ToArray());
        Assert.Equal(
            scenario.GetProperty("expectedRetainedSourceKeys")
                .EnumerateArray().Select(static item => item.GetString()!)
                .Order(StringComparer.Ordinal),
            observed.Select(static item => item.Status.Source)
                .Distinct(StringComparer.Ordinal)
                .Order(StringComparer.Ordinal));
        foreach (var removed in scenario.GetProperty("expectedRemovedSourceKeys")
                     .EnumerateArray().Select(static item => item.GetString()!))
            Assert.DoesNotContain(observed, item => item.Status.Source == removed);

        var expectedLoss = scenario.GetProperty("expectedLoss");
        Assert.All(observed, item =>
        {
            Assert.Equal(
                ulong.Parse(expectedLoss.GetProperty("coalescedIntermediateCount").GetString()!),
                item.Loss.CoalescedCount);
            Assert.Equal(
                ulong.Parse(expectedLoss.GetProperty("discardedTerminalCount").GetString()!),
                item.Loss.DiscardedTerminalCount);
        });
    }

    [Fact]
    public void Runtime_observation_loss_counters_saturate_independently()
    {
        using var document = Load("runtime-observation-v1.json");
        var scenario = document.RootElement.GetProperty("scenarios")[1];
        var initial = scenario.GetProperty("initialLoss");
        var increments = scenario.GetProperty("increments");
        var expected = scenario.GetProperty("expectedLoss");
        var coalesced = ulong.Parse(
            initial.GetProperty("coalescedIntermediateCount").GetString()!);
        var discarded = ulong.Parse(
            initial.GetProperty("discardedTerminalCount").GetString()!);

        for (var index = 0;
             index < increments.GetProperty("coalescedIntermediateCount").GetInt32();
             index++)
            coalesced = ZLinkObservationQueue<FixtureStatus>
                .IncrementLossCounter(coalesced);
        for (var index = 0;
             index < increments.GetProperty("discardedTerminalCount").GetInt32();
             index++)
            discarded = ZLinkObservationQueue<FixtureStatus>
                .IncrementLossCounter(discarded);

        Assert.Equal(
            ulong.Parse(expected.GetProperty("coalescedIntermediateCount").GetString()!),
            coalesced);
        Assert.Equal(
            ulong.Parse(expected.GetProperty("discardedTerminalCount").GetString()!),
            discarded);
    }

    private static string ObserveNestedResult(
        string target,
        ZLinkNestedRequestTerminator terminator)
    {
        var failure = Record.Exception(() =>
        {
            if (target == "sameSpot")
                ZLinkApplicationExecutionContext.ValidateSpotRequest(
                    "spot-A",
                    terminator);
            else
                ZLinkApplicationExecutionContext.ValidateActorRequest(
                    target switch
                    {
                        "selfActor" => "actor-A",
                        "differentMemberActorOnSameSpot" => "actor-B",
                        _ => "actor-C"
                    },
                    terminator);
        });
        if (failure is ZLinkFrameworkException
            { Kind: ZLinkFrameworkErrorKind.InvalidOperation })
            return "invalidOperation";
        Assert.Null(failure);
        return terminator == ZLinkNestedRequestTerminator.Yield
            ? "resumeOnNewTurn"
            : "awaitWithoutGateRelease";
    }

    private static JsonElement Scenario(JsonElement scenarios, string name) =>
        scenarios.EnumerateArray().Single(item =>
            item.GetProperty("name").GetString() == name);

    private static ZLinkSerialExecutionQueue CreateQueue(
        ZLinkRuntimeErrorSink errorSink) =>
        new(
            new ZLinkRuntimeTaskRunner(errorSink, CancellationToken.None),
            errorSink,
            CancellationToken.None);

    private static TaskCompletionSource Signal() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private static JsonDocument Load(string name)
    {
        var path = Path.Combine(
            Common.FrameworkTestEnvironment.GetRepoRoot(),
            "framework",
            "runtime",
            "conformance",
            name);
        return JsonDocument.Parse(File.ReadAllText(path));
    }

    private sealed record FixtureStatus(
        string Source,
        ulong Sequence,
        string Value);
}
