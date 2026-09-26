using System.Collections.Concurrent;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;

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
            ZLinkExecutionLanePolicy.Default.OwnerTimeBudget.TotalMilliseconds,
            limits.GetProperty("ownerTimeBudgetMilliseconds").GetInt32()
        );
        Assert.Equal(
            ZLinkExecutionLanePolicy.Default.LifecycleBurstLimit,
            limits.GetProperty("lifecycleBurstLimit").GetInt32()
        );
        Assert.Equal(8, ZLinkExecutionLanePolicy.Default.LifecycleBurstLimit);
        Assert.Equal(
            TimeSpan.FromMilliseconds(10),
            ZLinkExecutionLanePolicy.Default.OwnerTimeBudget
        );
    }

    [Fact]
    public void Serial_work_fifo_preserves_order_and_detaches_items_for_reappend()
    {
        var first = new ZLinkSerialWorkItem(static _ => ValueTask.CompletedTask);
        var second = new ZLinkSerialWorkItem(static _ => ValueTask.CompletedTask);
        var third = new ZLinkSerialWorkItem(static _ => ValueTask.CompletedTask);
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
        var warmupItem = new ZLinkSerialWorkItem(static _ => ValueTask.CompletedTask);
        warmup.Enqueue(warmupItem);
        Assert.True(warmup.TryDequeue(out _));

        var queue = new ZLinkSerialWorkQueue();
        var first = new ZLinkSerialWorkItem(static _ => ValueTask.CompletedTask);
        var second = new ZLinkSerialWorkItem(static _ => ValueTask.CompletedTask);
        var third = new ZLinkSerialWorkItem(static _ => ValueTask.CompletedTask);
        var before = GC.GetAllocatedBytesForCurrentThread();

        queue.Enqueue(first);
        queue.Enqueue(second);
        queue.Enqueue(third);

        var after = GC.GetAllocatedBytesForCurrentThread();
        Assert.Equal(before, after);
    }

    [Fact]
    public async Task Serial_lanes_accept_work_beyond_the_former_capacity_boundaries()
    {
        using var document = Load("serial-execution-v1.json");
        var scenarios = document.RootElement.GetProperty("accountingScenarios");
        var applicationCount = Scenario(scenarios, "application-count-boundary")
            .GetProperty("acceptedWorkCount")
            .GetInt32();
        var lifecycleCount = Scenario(scenarios, "lifecycle-count-boundary")
            .GetProperty("acceptedWorkCount")
            .GetInt32();
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var queue = CreateQueue(errorSink);
        var firstStarted = Signal();
        var releaseFirst = Signal();

        try
        {
            Assert.Equal(
                ZLinkSerialPostAdmission.Accepted,
                queue.TryPostApplicationWithAdmission(
                    async _ =>
                    {
                        firstStarted.TrySetResult();
                        await releaseFirst.Task.ConfigureAwait(false);
                    },
                    out _
                )
            );
            await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            for (var index = 1; index < applicationCount; index++)
                Assert.Equal(
                    ZLinkSerialPostAdmission.Accepted,
                    queue.TryPostApplicationWithAdmission(
                        static _ => ValueTask.CompletedTask,
                        out _
                    )
                );
            Assert.Equal(
                ZLinkSerialPostAdmission.Accepted,
                queue.TryPostApplicationWithAdmission(
                    static _ => ValueTask.CompletedTask,
                    out var extraApplication
                )
            );
            Assert.Equal(applicationCount + 1, queue.ApplicationPendingCount);

            ZLinkSerialWorkItem? lastLifecycle = null;
            for (var index = 0; index < lifecycleCount; index++)
            {
                Assert.Equal(
                    ZLinkSerialPostAdmission.Accepted,
                    queue.TryPostNextWithAdmission(
                        static _ => ValueTask.CompletedTask,
                        out var accepted
                    )
                );
                lastLifecycle = accepted;
            }
            Assert.Equal(
                ZLinkSerialPostAdmission.Accepted,
                queue.TryPostNextWithAdmission(
                    static _ => ValueTask.CompletedTask,
                    out var extraLifecycle
                )
            );
            Assert.Equal(lifecycleCount + 1, queue.LifecyclePendingCount);

            releaseFirst.TrySetResult();
            await queue.ApplicationDrained.WaitAsync(TimeSpan.FromSeconds(10));
            await Task.WhenAll(
                    lastLifecycle!.Completion,
                    extraApplication.Completion,
                    extraLifecycle.Completion
                )
                .WaitAsync(TimeSpan.FromSeconds(10));
        }
        finally
        {
            releaseFirst.TrySetResult();
        }
    }

    [Fact]
    public async Task Serial_byte_metadata_does_not_limit_either_lane()
    {
        using var document = Load("serial-execution-v1.json");
        var scenarios = document.RootElement.GetProperty("accountingScenarios");
        var applicationBytes = Scenario(scenarios, "application-byte-boundary")
            .GetProperty("retainedPayloadBytesPerWork")
            .GetInt64();
        var lifecycleBytes = Scenario(scenarios, "lifecycle-byte-boundary")
            .GetProperty("retainedPayloadBytesPerWork")
            .GetInt64();
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
                    payloadBytes: applicationBytes,
                    metadataBytes: 0,
                    transferred: false,
                    out var application
                )
            );
            Assert.Equal(
                ZLinkSerialPostAdmission.Accepted,
                queue.TryPostApplicationWithAdmission(
                    static _ => ValueTask.CompletedTask,
                    payloadBytes: 0,
                    metadataBytes: 0,
                    transferred: false,
                    out var queuedApplication
                )
            );

            Assert.Equal(
                ZLinkSerialPostAdmission.Accepted,
                queue.TryPostNextWithAdmission(
                    async _ => await releaseLifecycle.Task.ConfigureAwait(false),
                    payloadBytes: lifecycleBytes,
                    metadataBytes: 0,
                    transferred: false,
                    out var lifecycle
                )
            );
            Assert.Equal(
                ZLinkSerialPostAdmission.Accepted,
                queue.TryPostNextWithAdmission(
                    static _ => ValueTask.CompletedTask,
                    payloadBytes: 0,
                    metadataBytes: 0,
                    transferred: false,
                    out var queuedLifecycle
                )
            );

            releaseApplication.TrySetResult();
            releaseLifecycle.TrySetResult();
            await Task.WhenAll(
                    application.Completion,
                    lifecycle.Completion,
                    queuedApplication.Completion,
                    queuedLifecycle.Completion
                )
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
        Assert.True(
            document
                .RootElement.GetProperty("admissionInvariants")
                .GetProperty("enqueueFailureRestoresReservation")
                .GetBoolean()
        );
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errorSink, CancellationToken.None),
            errorSink,
            CancellationToken.None
        );
        var release = Signal();
        try
        {
            Assert.Equal(
                ZLinkAcceptedWorkAdmission.Accepted,
                queue.TryPostAccepted(
                    new byte[5],
                    async _ => await release.Task.ConfigureAwait(false),
                    static () => { },
                    out var first
                )
            );
            Assert.Equal(1UL, first.AcceptedSequence);
            Assert.Equal(1, queue.ApplicationPendingCount);
            Assert.Equal(
                ZLinkAcceptedWorkAdmission.Accepted,
                queue.TryPostAccepted(
                    new byte[13],
                    static _ => ValueTask.CompletedTask,
                    static () => { },
                    out _
                )
            );
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
                    out var second
                )
            );
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
        var expected = scenario
            .GetProperty("expectedSelection")
            .EnumerateArray()
            .Select(static item => item.GetString()!)
            .ToArray();
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var queue = CreateQueue(errorSink);
        var blockerStarted = Signal();
        var releaseBlocker = Signal();
        var selected = new ConcurrentQueue<string>();

        Assert.True(
            queue.TryPost(
                async _ =>
                {
                    blockerStarted.TrySetResult();
                    await releaseBlocker.Task.ConfigureAwait(false);
                },
                out _
            )
        );
        await blockerStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var completions = new List<Task>();
        foreach (
            var name in scenario
                .GetProperty("lifecycleInput")
                .EnumerateArray()
                .Select(static item => item.GetString()!)
        )
        {
            Assert.True(
                queue.TryPostNext(
                    _ =>
                    {
                        selected.Enqueue(name);
                        return ValueTask.CompletedTask;
                    },
                    out var item
                )
            );
            completions.Add(item.Completion);
        }
        foreach (
            var name in scenario
                .GetProperty("applicationInput")
                .EnumerateArray()
                .Select(static item => item.GetString()!)
        )
        {
            Assert.True(
                queue.TryPostApplication(
                    _ =>
                    {
                        selected.Enqueue(name);
                        return ValueTask.CompletedTask;
                    },
                    out var item
                )
            );
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
            CancellationToken.None
        );
        var completed = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        _insideAdmissionCall = true;
        Assert.True(
            queue.TryPost(
                _ =>
                {
                    completed.TrySetResult(_insideAdmissionCall);
                    return ValueTask.CompletedTask;
                },
                out _
            )
        );
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
                IsMemberActor: static candidate => candidate == "actor-B"
            )
        );

        foreach (
            var scenario in document.RootElement.GetProperty("sameOwnerCalls").EnumerateArray()
        )
        {
            var target = scenario.GetProperty("target").GetString()!;
            Assert.Equal(
                scenario.GetProperty("async").GetString(),
                ObserveNestedResult(target, ZLinkNestedRequestTerminator.Async)
            );
            Assert.Equal(
                scenario.GetProperty("yield").GetString(),
                ObserveNestedResult(target, ZLinkNestedRequestTerminator.Yield)
            );
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
            limits.GetProperty("defaultTerminalCapacity").GetInt32()
        );
        Assert.Equal(
            ulong.Parse(limits.GetProperty("signedLossCounterMaximum").GetString()!),
            ZLinkObservationQueue<FixtureStatus>.IncrementLossCounter(ulong.MaxValue)
        );

        var scenario = root.GetProperty("scenarios")[0];
        var queue = new ZLinkObservationQueue<FixtureStatus>(
            static status => status.Source,
            scenario.GetProperty("terminalCapacity").GetInt32()
        );
        foreach (var operation in scenario.GetProperty("operations").EnumerateArray())
        {
            queue.Publish(
                new FixtureStatus(
                    operation.GetProperty("source").GetString()!,
                    operation.GetProperty("sequence").GetUInt64(),
                    operation.GetProperty("value").GetString()!
                ),
                operation.GetProperty("kind").GetString() == "terminal"
            );
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
                StringComparer.Ordinal
            );
        var expectedTerminal = scenario
            .GetProperty("expectedTerminalFifo")
            .EnumerateArray()
            .Select(static item => item.GetProperty("value").GetString()!)
            .ToArray();
        Assert.Equal(
            expectedIntermediate,
            observed
                .Where(item => expectedIntermediate.ContainsKey(item.Status.Source))
                .ToDictionary(
                    static item => item.Status.Source,
                    static item => item.Status.Value,
                    StringComparer.Ordinal
                )
        );
        Assert.Equal(
            expectedTerminal,
            observed
                .Where(item => !expectedIntermediate.ContainsKey(item.Status.Source))
                .Select(static item => item.Status.Value)
                .ToArray()
        );
        Assert.Equal(
            scenario
                .GetProperty("expectedRetainedSourceKeys")
                .EnumerateArray()
                .Select(static item => item.GetString()!)
                .Order(StringComparer.Ordinal),
            observed
                .Select(static item => item.Status.Source)
                .Distinct(StringComparer.Ordinal)
                .Order(StringComparer.Ordinal)
        );
        foreach (
            var removed in scenario
                .GetProperty("expectedRemovedSourceKeys")
                .EnumerateArray()
                .Select(static item => item.GetString()!)
        )
            Assert.DoesNotContain(observed, item => item.Status.Source == removed);

        var expectedLoss = scenario.GetProperty("expectedLoss");
        Assert.All(
            observed,
            item =>
            {
                Assert.Equal(
                    ulong.Parse(
                        expectedLoss.GetProperty("coalescedIntermediateCount").GetString()!
                    ),
                    item.Loss.CoalescedCount
                );
                Assert.Equal(
                    ulong.Parse(expectedLoss.GetProperty("discardedTerminalCount").GetString()!),
                    item.Loss.DiscardedTerminalCount
                );
            }
        );
    }

    [Fact]
    public void Runtime_observation_loss_counters_saturate_independently()
    {
        using var document = Load("runtime-observation-v1.json");
        var scenario = document.RootElement.GetProperty("scenarios")[1];
        var initial = scenario.GetProperty("initialLoss");
        var increments = scenario.GetProperty("increments");
        var expected = scenario.GetProperty("expectedLoss");
        var coalesced = ulong.Parse(initial.GetProperty("coalescedIntermediateCount").GetString()!);
        var discarded = ulong.Parse(initial.GetProperty("discardedTerminalCount").GetString()!);

        for (
            var index = 0;
            index < increments.GetProperty("coalescedIntermediateCount").GetInt32();
            index++
        )
            coalesced = ZLinkObservationQueue<FixtureStatus>.IncrementLossCounter(coalesced);
        for (
            var index = 0;
            index < increments.GetProperty("discardedTerminalCount").GetInt32();
            index++
        )
            discarded = ZLinkObservationQueue<FixtureStatus>.IncrementLossCounter(discarded);

        Assert.Equal(
            ulong.Parse(expected.GetProperty("coalescedIntermediateCount").GetString()!),
            coalesced
        );
        Assert.Equal(
            ulong.Parse(expected.GetProperty("discardedTerminalCount").GetString()!),
            discarded
        );
    }

    private static string ObserveNestedResult(
        string target,
        ZLinkNestedRequestTerminator terminator
    )
    {
        var failure = Record.Exception(() =>
        {
            if (target == "sameSpot")
                ZLinkApplicationExecutionContext.ValidateSpotRequest("spot-A", terminator);
            else
                ZLinkApplicationExecutionContext.ValidateActorRequest(
                    target switch
                    {
                        "selfActor" => "actor-A",
                        "differentMemberActorOnSameSpot" => "actor-B",
                        _ => "actor-C",
                    },
                    terminator
                );
        });
        if (failure is ZLinkFrameworkException { Kind: ZLinkFrameworkErrorKind.InvalidOperation })
            return "invalidOperation";
        Assert.Null(failure);
        return terminator == ZLinkNestedRequestTerminator.Yield
            ? "resumeOnNewTurn"
            : "awaitWithoutGateRelease";
    }

    private static JsonElement Scenario(JsonElement scenarios, string name) =>
        scenarios.EnumerateArray().Single(item => item.GetProperty("name").GetString() == name);

    private static ZLinkSerialExecutionQueue CreateQueue(ZLinkRuntimeErrorSink errorSink) =>
        new(
            new ZLinkRuntimeTaskRunner(errorSink, CancellationToken.None),
            errorSink,
            CancellationToken.None
        );

    private static TaskCompletionSource Signal() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    [Fact]
    public async Task Route_mesh_placement_counts_follow_the_reporting_mesh_node()
    {
        using var document = Load("route-mesh-placement-v1.json");
        var fixture = document.RootElement;
        Assert.Equal(
            "zlink.framework.route-mesh-placement",
            fixture.GetProperty("fixture").GetString()
        );
        Assert.Equal(1, fixture.GetProperty("version").GetInt32());
        var lease = fixture.GetProperty("ownerLease");
        // Every scenario runs so one report names all divergent scenarios.
        var failures = new List<string>();
        foreach (var scenario in fixture.GetProperty("scenarios").EnumerateArray())
        {
            try
            {
                await RunPlacementScenarioAsync(lease, scenario);
            }
            catch (Exception error)
            {
                failures.Add($"{scenario.GetProperty("name").GetString()}: {error.Message}");
            }
        }
        Assert.True(failures.Count == 0, string.Join(Environment.NewLine, failures));
    }

    private static async Task RunPlacementScenarioAsync(JsonElement lease, JsonElement scenario)
    {
        var name = scenario.GetProperty("name").GetString()!;
        var suffix = Guid.NewGuid().ToString("N");
        var nodes = scenario.GetProperty("meshNodes").EnumerateArray().ToArray();
        Assert.True(nodes.Length <= PlacementSpotTypes.Length, name);
        Assert.True(
            nodes.Count(static node => node.GetProperty("instanceSpotFactory").GetBoolean()) <= 1,
            name
        );
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            if (nodes.Any(static node => node.GetProperty("instanceSpotFactory").GetBoolean()))
                options.AddRelocationStore(new InMemoryRelocationStore());
            var locations = options.ConfigureLocations();
            locations.PollingInterval = TimeSpan.FromMilliseconds(10);
            locations.OwnerLeaseRenewInterval = Millis(lease, "renewIntervalMs");
            locations.OwnerLeaseRenewTimeout = Millis(lease, "renewTimeoutMs");
            locations.OwnerLeaseTtl = Millis(lease, "ttlMs");
            locations.OwnerLeaseFencingMargin = Millis(lease, "fencingMarginMs");
            for (var index = 0; index < nodes.Length; index++)
            {
                var meshName = nodes[index].GetProperty("meshName").GetString()!;
                Assert.Equal("disabled", nodes[index].GetProperty("spotRelocation").GetString());
                var objects = options
                    .AddRouteMesh(meshName)
                    .Listen($"inproc://placement-{meshName}-{suffix}")
                    .SetActorLimit(nodes[index].GetProperty("actorLimit").GetInt32())
                    .SetSpotLimit(nodes[index].GetProperty("spotLimit").GetInt32())
                    .SetActivationConcurrency(
                        nodes[index].GetProperty("activationConcurrency").GetInt32()
                    )
                    .Objects()
                    .Server();
                PlacementSpotTypes[index](objects, $"placement-spot-{meshName}");
                if (nodes[index].GetProperty("actorFactory").GetBoolean())
                    objects.AddActorFactory<PlacementActor, PlacementActorFactory>(
                        "placement-actor",
                        static factory => factory.DisableRelocation()
                    );
                if (nodes[index].GetProperty("instanceSpotFactory").GetBoolean())
                    objects.AddInstanceSpotFactory<PlacementInstanceSpot>(
                        "placement-instance",
                        static factory => factory.DisableRelocation()
                    );
            }
        });
        await using var provider = services.BuildServiceProvider();
        var hosted = provider
            .GetServices<IHostedService>()
            .Single(static service => service is ZLinkFrameworkHostedService);
        await hosted.StartAsync(CancellationToken.None);
        var held = new List<(PlacementHold Hold, Task Operation)>();
        try
        {
            var spots = provider.GetRequiredService<IZLinkSpotManager>();
            var actors = provider.GetRequiredService<IZLinkActorManager>();
            var spotClient = provider.GetRequiredService<IZLinkSpotClient>();
            var actorIndex = 0;
            var instanceIndex = 0;
            string? lastActorId = null;
            string? lastSpotId = null;
            foreach (var entry in scenario.GetProperty("objects").EnumerateArray())
            {
                var meshName = entry.GetProperty("meshName").GetString()!;
                var kind = entry.GetProperty("kind").GetString()!;
                var hold = entry.GetProperty("hold").GetBoolean() ? new PlacementHold(kind) : null;
                for (var count = 0; count < entry.GetProperty("count").GetInt32(); count++)
                {
                    PlacementHold.Current = hold;
                    Task operation;
                    switch (kind)
                    {
                        case "actor":
                            var actorId = $"{name}-actor-{actorIndex++}";
                            lastActorId = actorId;
                            operation = actors
                                .Create(actorId, "placement-actor")
                                .InMesh(meshName)
                                .Timeout(TimeSpan.FromSeconds(10))
                                .Async()
                                .AsTask();
                            break;
                        case "userSpot":
                            operation = CreateUserSpotAsync(
                                spots,
                                meshName,
                                spotId => lastSpotId = spotId
                            );
                            break;
                        case "instanceSpot":
                            operation = spotClient
                                .SendToSpot(
                                    $"{name}-instance-{instanceIndex++}",
                                    new PlacementPing(name)
                                )
                                .InstanceSpot("placement-instance")
                                .InMesh(meshName)
                                .Async()
                                .AsTask();
                            break;
                        case "actorJoin":
                            operation = JoinSpotAsync(
                                PlacementActor.Get(
                                    lastActorId ?? throw new InvalidOperationException(name)
                                ),
                                lastSpotId ?? throw new InvalidOperationException(name)
                            );
                            break;
                        default:
                            throw new InvalidOperationException($"{name}: object kind {kind}");
                    }
                    if (hold is null)
                    {
                        await WithinAsync(operation, $"{name}: {kind}");
                        continue;
                    }
                    // The held operation stays in flight while the expectations are checked.
                    await WithinAsync(
                        Task.WhenAny(hold.Entered.Task, operation),
                        $"{name}: held {kind}"
                    );
                    Assert.True(hold.Entered.Task.IsCompleted, $"{name}: {kind} did not start");
                    held.Add((hold, operation));
                }
            }
            var runtimeOptions = provider.GetRequiredService<IZLinkRouteMeshRuntimeOptions>();
            foreach (var node in nodes)
                runtimeOptions.Mesh(node.GetProperty("meshName").GetString()!).PlacementWeight =
                    node.GetProperty("placementWeightAfterStartup").GetInt32();
            var runtime = provider.GetRequiredService<IZLinkRouteMeshRuntime>();
            foreach (var expected in scenario.GetProperty("expected").EnumerateArray())
            {
                var meshName = expected.GetProperty("meshName").GetString()!;
                var expectedAvailable = expected.GetProperty("isAvailable").GetBoolean();
                var expectedState =
                    expected.GetProperty("state").GetString() == "degraded"
                        ? ZLinkTopologyState.Degraded
                        : ZLinkTopologyState.Ready;
                using var observationTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
                await using var observer = runtime
                    .ObserveAsync(meshName, observationTimeout.Token)
                    .GetAsyncEnumerator(observationTimeout.Token);
                Assert.True(await observer.MoveNextAsync(), $"{name}:{meshName}: no status observed");
                var status = observer.Current.Status;
                while (status.Placement.IsAvailable != expectedAvailable || status.State != expectedState)
                {
                    Assert.True(await observer.MoveNextAsync(), $"{name}:{meshName}: observation ended");
                    status = observer.Current.Status;
                }
                var label = $"{name}:{meshName}";
                Assert.True(
                    expected.GetProperty("activeActorCount").GetInt32()
                        == status.Placement.ActiveActorCount,
                    $"{label}:activeActorCount={status.Placement.ActiveActorCount}"
                );
                Assert.True(
                    expected.GetProperty("activeSpotCount").GetInt32()
                        == status.Placement.ActiveSpotCount,
                    $"{label}:activeSpotCount={status.Placement.ActiveSpotCount}"
                );
                Assert.True(
                    expectedAvailable == status.Placement.IsAvailable,
                    $"{label}:isAvailable={status.Placement.IsAvailable}"
                );
                Assert.True(expectedState == status.State, $"{label}:state={status.State}");
            }
        }
        finally
        {
            PlacementHold.Current = null;
            foreach (var (hold, _) in held)
                hold.Release.TrySetResult();
            foreach (var (hold, operation) in held)
                await WithinAsync(operation, $"{name}: released {hold.Kind}");
            await hosted.StopAsync(CancellationToken.None);
        }
    }

    private static async Task WithinAsync(Task operation, string label)
    {
        try
        {
            await operation.WaitAsync(TimeSpan.FromSeconds(10));
        }
        catch (TimeoutException error)
        {
            throw new TimeoutException($"{label} did not complete.", error);
        }
    }

    private static async Task CreateUserSpotAsync(
        IZLinkSpotManager spots,
        string meshName,
        Action<string> created
    )
    {
        var result = await spots
            .Create($"placement-spot-{meshName}")
            .InMesh(meshName)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async();
        created(result.Spot.SpotId);
    }

    private static async Task JoinSpotAsync(PlacementActor actor, string spotId)
    {
        var joined = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        actor.JoinCompleted = joined;
        // Stands in for the Framework-managed handler turn that owns a deferred Join.
        using (var handler = ZLinkDeferredActorJoinHandlerScope.Open())
        {
            actor.Context.JoinSpot(spotId).Timeout(TimeSpan.FromSeconds(10)).Defer();
            handler.Complete();
        }
        await joined.Task;
    }

    private static TimeSpan Millis(JsonElement lease, string field) =>
        TimeSpan.FromMilliseconds(lease.GetProperty(field).GetInt32());

    // Spot factory types are unique per host, so each fixture MeshNode gets its own type.
    private static readonly Action<IZLinkMeshObjectServerBuilder, string>[] PlacementSpotTypes =
    [
        static (objects, stableType) =>
            objects.AddSpotFactory<FirstPlacementSpot>(
                stableType,
                static factory => factory.DisableRelocation()
            ),
        static (objects, stableType) =>
            objects.AddSpotFactory<SecondPlacementSpot>(
                stableType,
                static factory => factory.DisableRelocation()
            ),
    ];

    /// <summary>
    /// Keeps one fixture operation in flight: the matching hook signals <see cref="Entered"/>
    /// and waits for <see cref="Release"/>.
    /// </summary>
    private sealed class PlacementHold(string kind)
    {
        private static PlacementHold? _current;

        internal static PlacementHold? Current
        {
            get => Volatile.Read(ref _current);
            set => Volatile.Write(ref _current, value);
        }

        internal TaskCompletionSource Entered { get; } = Signal();

        internal TaskCompletionSource Release { get; } = Signal();

        internal static async ValueTask WaitAsync(string kind)
        {
            if (Current is not { } hold || hold.Kind != kind)
                return;
            hold.Entered.TrySetResult();
            await hold.Release.Task.ConfigureAwait(false);
        }

        internal string Kind { get; } = kind;
    }

    private abstract class PlacementSpot(IZLinkSpotContext context) : IZLinkSpot<PlacementActor>
    {
        public IZLinkSpotContext Context { get; } = context;

        public async ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
            ZLinkMessage request,
            CancellationToken cancellationToken
        )
        {
            await PlacementHold.WaitAsync("userSpot");
            return ZLinkSpotCreateResponse.Accept();
        }

        public async ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken
        )
        {
            await PlacementHold.WaitAsync("actorJoin");
            return ZLinkSpotActorJoinResult.Accept();
        }

        public ValueTask OnJoinedActorAsync(
            PlacementActor actor,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(
            PlacementActor actor,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;
    }

    private sealed class FirstPlacementSpot(IZLinkSpotContext context) : PlacementSpot(context);

    private sealed class SecondPlacementSpot(IZLinkSpotContext context) : PlacementSpot(context);

    private sealed class PlacementInstanceSpot(IZLinkInstanceSpotContext context)
        : IZLinkInstanceSpot
    {
        public IZLinkInstanceSpotContext Context { get; } = context;

        public ValueTask OnInitializeAsync(CancellationToken cancellationToken) =>
            PlacementHold.WaitAsync("instanceSpot");
    }

    private sealed record PlacementPing(string Value);

    private sealed class PlacementActor(IZLinkActorContext context) : IZLinkActor
    {
        private static readonly ConcurrentDictionary<string, PlacementActor> Created = new();

        public IZLinkActorContext Context { get; } = context;

        internal TaskCompletionSource? JoinCompleted { get; set; }

        internal static PlacementActor Get(string actorId) => Created[actorId];

        internal static PlacementActor Register(PlacementActor actor)
        {
            Created[actor.Context.ActorId] = actor;
            return actor;
        }

        public ValueTask OnJoinCompletedAsync(
            ZLinkActorJoinCompletion completion,
            CancellationToken cancellationToken
        )
        {
            if (completion is ZLinkActorJoinCompletion.Accepted)
                JoinCompleted?.TrySetResult();
            else
                JoinCompleted?.TrySetException(
                    new InvalidOperationException($"Actor join completed as {completion}.")
                );
            return ValueTask.CompletedTask;
        }
    }

    private sealed class PlacementActorFactory : IZLinkActorFactory<PlacementActor>
    {
        public async ValueTask<PlacementActor> CreateAsync(
            IZLinkActorContext context,
            CancellationToken cancellationToken = default
        )
        {
            await PlacementHold.WaitAsync("actor");
            return PlacementActor.Register(new PlacementActor(context));
        }
    }

    private static JsonDocument Load(string name)
    {
        var path = Path.Combine(
            Common.FrameworkTestEnvironment.GetRepoRoot(),
            "framework",
            "runtime",
            "conformance",
            name
        );
        return JsonDocument.Parse(File.ReadAllText(path));
    }

    private sealed record FixtureStatus(string Source, ulong Sequence, string Value);
}
