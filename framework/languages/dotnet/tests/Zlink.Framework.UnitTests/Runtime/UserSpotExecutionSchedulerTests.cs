using System.Collections.Concurrent;
using Zlink.Framework.Runtime.Backend.Contracts;

namespace Zlink.Framework.UnitTests;

public sealed class UserSpotExecutionSchedulerTests
{
    [Fact]
    public void FactoryRegistrationFixesExecutionModeAndRejectsInvalidValues()
    {
        var registration = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "execution-node"
        };
        IZLinkMeshObjectServerBuilder server = new ZLinkMeshNodeBuilder(registration)
            .Objects()
            .Server();

        server.AddSpotFactory<ExecutionTestSpot>(
            "execution.spot",
            factory => factory
                .StableTypeLimit(8)
                .ExecutionMode(ZLinkUserSpotExecutionMode.PerActor)
                .RelocationReadiness(
                    ZLinkSpotRelocationReadinessMode.AnyTurnBoundary)
                .RecreateOnRelocation());

        var configured = registration.UserSpotFactoryOptions[typeof(ExecutionTestSpot)];
        Assert.Equal(8, configured.StableTypeLimit);
        Assert.Equal(ZLinkUserSpotExecutionMode.PerActor, configured.ExecutionMode);
        Assert.Equal(
            ZLinkSpotRelocationReadinessMode.AnyTurnBoundary,
            configured.RelocationReadiness);

        var invalidRegistration = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "invalid-execution-node"
        };
        IZLinkMeshObjectServerBuilder invalid = new ZLinkMeshNodeBuilder(invalidRegistration)
            .Objects()
            .Server();
        Assert.Throws<ZLinkConfigurationException>(() =>
            invalid.AddSpotFactory<OtherExecutionTestSpot>(
                "invalid.execution.spot",
                factory => factory
                    .ExecutionMode((ZLinkUserSpotExecutionMode)9)
                    .DisableRelocation()));

        Assert.Throws<ZLinkConfigurationException>(() =>
            invalid.AddSpotFactory<OtherExecutionTestSpot>(
                "invalid.application-signaled.spot",
                factory => factory
                    .ExecutionMode(ZLinkUserSpotExecutionMode.PerActor)
                    .RelocationReadiness(
                        ZLinkSpotRelocationReadinessMode.ApplicationSignaled)
                    .DisableRelocation()));
    }

    [Fact]
    public async Task SpotWide_Yield_ReleasesSpotGateButKeepsActorFifoClaim()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(errorSink, ZLinkUserSpotExecutionMode.SpotWide);
        var externalStarted = NewSignal();
        var completeExternal = NewSignal();
        var firstResumed = NewSignal();
        var sameActorSecondRan = NewSignal();
        var otherActorRan = NewSignal();
        var spotRan = NewSignal();
        var order = new ConcurrentQueue<string>();

        var first = executor.ExecuteActorAsync(
            "actor-1",
            async static (_, state, ct) =>
            {
                state.Order.Enqueue("actor-1-start");
                var turn = ZLinkApplicationExecutionContext.RequireYieldTurn("test request");
                await turn.YieldFrameworkCallAsync(
                        async _ =>
                        {
                            state.ExternalStarted.TrySetResult();
                            await state.CompleteExternal.Task.ConfigureAwait(false);
                        },
                        ct)
                    .ConfigureAwait(false);
                state.Order.Enqueue("actor-1-resumed");
                state.FirstResumed.TrySetResult();
            },
            new YieldState(order, externalStarted, completeExternal, firstResumed),
            CancellationToken.None).AsTask();
        await externalStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var sameActorSecond = RecordActor(
            executor,
            "actor-1",
            order,
            "actor-1-second",
            sameActorSecondRan);
        var otherActor = RecordActor(executor, "actor-2", order, "actor-2", otherActorRan);
        var spot = executor.ExecuteAsync(
            (_, _) =>
            {
                order.Enqueue("spot");
                spotRan.TrySetResult();
                return ValueTask.CompletedTask;
            },
            CancellationToken.None).AsTask();

        await Task.WhenAll(otherActorRan.Task, spotRan.Task).WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(sameActorSecondRan.Task.IsCompleted);

        completeExternal.TrySetResult();
        await Task.WhenAll(first, sameActorSecond, otherActor, spot)
            .WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(firstResumed.Task.IsCompleted);
        var recorded = order.ToArray();
        Assert.True(
            Array.IndexOf(recorded, "actor-1-resumed")
            < Array.IndexOf(recorded, "actor-1-second"));
    }

    [Fact]
    public async Task PerActor_UsesIndependentActorSpotAndTimerLanesWithLaneFifo()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(errorSink, ZLinkUserSpotExecutionMode.PerActor);
        var actorOneStarted = NewSignal();
        var releaseActorOne = NewSignal();
        var actorOneSecondRan = NewSignal();
        var actorTwoRan = NewSignal();
        var spotRan = NewSignal();
        var timerOneStarted = NewSignal();
        var releaseTimerOne = NewSignal();
        var timerOneSecondRan = NewSignal();
        var timerTwoRan = NewSignal();

        var actorOne = executor.ExecuteActorAsync(
            "actor-1",
            async static (_, state, _) =>
            {
                state.Started.TrySetResult();
                await state.Release.Task.ConfigureAwait(false);
            },
            new BlockState(actorOneStarted, releaseActorOne),
            CancellationToken.None).AsTask();
        var timerOne = executor.ExecuteTimerAsync(
            "timer-1",
            async static (_, state, _) =>
            {
                state.Started.TrySetResult();
                await state.Release.Task.ConfigureAwait(false);
            },
            new BlockState(timerOneStarted, releaseTimerOne),
            CancellationToken.None).AsTask();
        await Task.WhenAll(actorOneStarted.Task, timerOneStarted.Task)
            .WaitAsync(TimeSpan.FromSeconds(5));

        var actorOneSecond = RecordActor(
            executor,
            "actor-1",
            null,
            null,
            actorOneSecondRan);
        var actorTwo = RecordActor(executor, "actor-2", null, null, actorTwoRan);
        var spot = executor.ExecuteAsync(
            (_, _) =>
            {
                spotRan.TrySetResult();
                return ValueTask.CompletedTask;
            },
            CancellationToken.None).AsTask();
        var timerOneSecond = RecordTimer(executor, "timer-1", timerOneSecondRan);
        var timerTwo = RecordTimer(executor, "timer-2", timerTwoRan);

        await Task.WhenAll(actorTwoRan.Task, spotRan.Task, timerTwoRan.Task)
            .WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(actorOneSecondRan.Task.IsCompleted);
        Assert.False(timerOneSecondRan.Task.IsCompleted);

        releaseActorOne.TrySetResult();
        releaseTimerOne.TrySetResult();
        await Task.WhenAll(
                actorOne,
                actorOneSecond,
                actorTwo,
                spot,
                timerOne,
                timerOneSecond,
                timerTwo)
            .WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task PerActor_YieldIsRejectedBeforeOperationSubmission()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(errorSink, ZLinkUserSpotExecutionMode.PerActor);
        var submitCount = 0;

        var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(
            () => executor.ExecuteActorAsync(
                    "actor-1",
                    (_, _, _) =>
                    {
                        _ = ZLinkApplicationExecutionContext.RequireYieldTurn("Actor request");
                        Interlocked.Increment(ref submitCount);
                        return ValueTask.CompletedTask;
                    },
                    0,
                    CancellationToken.None)
                .AsTask());

        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, failure.Kind);
        Assert.Equal(0, Volatile.Read(ref submitCount));
    }

    [Fact]
    public async Task SpotWide_SameGateAwaitIsRejectedBeforeSubmission()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(errorSink, ZLinkUserSpotExecutionMode.SpotWide);
        var attempts = 0;

        await executor.ExecuteActorAsync(
            "actor-1",
            (_, _, _) =>
            {
                AssertInvalid(() =>
                {
                    ZLinkApplicationExecutionContext.RejectActorRequestWhenSameClaim("actor-1");
                    Interlocked.Increment(ref attempts);
                });
                using (ZLinkApplicationExecutionContext.Push(
                           new ZLinkApplicationExecutionScope(
                               "test-spot",
                               ZLinkUserSpotExecutionMode.SpotWide,
                               "actor-1",
                               YieldAllowed: true,
                               IsMemberActor: candidate => candidate == "actor-2")))
                    AssertInvalid(() =>
                    {
                        ZLinkApplicationExecutionContext
                            .RejectActorRequestWhenSameClaim("actor-2");
                        Interlocked.Increment(ref attempts);
                    });
                AssertInvalid(() =>
                {
                    ZLinkApplicationExecutionContext.RejectSpotRequestWhenSameGate("test-spot");
                    Interlocked.Increment(ref attempts);
                });
                AssertInvalid(() =>
                {
                    ZLinkApplicationExecutionContext.RejectActorJoinWhenSameGate("another-spot");
                    Interlocked.Increment(ref attempts);
                });
                return ValueTask.CompletedTask;
            },
            0,
            CancellationToken.None);

        Assert.Equal(0, Volatile.Read(ref attempts));
    }

    [Fact]
    public async Task PerActor_RelocationSeal_WaitsForEveryActorSpotAndTimerLane()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(
            errorSink,
            ZLinkUserSpotExecutionMode.PerActor);
        var actorStarted = NewSignal();
        var releaseActor = NewSignal();
        var spotStarted = NewSignal();
        var releaseSpot = NewSignal();
        var timerStarted = NewSignal();
        var releaseTimer = NewSignal();

        var actor = executor.ExecuteActorAsync(
            "actor-1",
            Block,
            new BlockState(actorStarted, releaseActor),
            CancellationToken.None).AsTask();
        var spot = executor.ExecuteAsync(
            Block,
            new BlockState(spotStarted, releaseSpot),
            CancellationToken.None).AsTask();
        var timer = executor.ExecuteTimerAsync(
            "timer-1",
            Block,
            new BlockState(timerStarted, releaseTimer),
            CancellationToken.None).AsTask();
        await Task.WhenAll(actorStarted.Task, spotStarted.Task, timerStarted.Task)
            .WaitAsync(TimeSpan.FromSeconds(5));

        var sealTask = executor.SealRelocationAsync(CancellationToken.None).AsTask();
        Assert.False(sealTask.IsCompleted);

        releaseSpot.TrySetResult();
        await spot.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(sealTask.IsCompleted);

        releaseActor.TrySetResult();
        await actor.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(sealTask.IsCompleted);

        releaseTimer.TrySetResult();
        await timer.WaitAsync(TimeSpan.FromSeconds(5));
        var seal = await sealTask.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(executor.TryAbortRelocation(seal));

        static async ValueTask Block(
            ZLinkSpotActivation _,
            BlockState state,
            CancellationToken __)
        {
            state.Started.TrySetResult();
            await state.Release.Task.ConfigureAwait(false);
        }
    }

    [Fact]
    public async Task SpotWide_RelocationSeal_WaitsForYieldedTerminalContinuation()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(
            errorSink,
            ZLinkUserSpotExecutionMode.SpotWide);
        var externalStarted = NewSignal();
        var completeExternal = NewSignal();
        var terminalContinuation = NewSignal();

        var operation = executor.ExecuteActorAsync(
            "actor-1",
            async static (_, state, ct) =>
            {
                var turn = ZLinkApplicationExecutionContext
                    .RequireYieldTurn("relocation barrier test");
                await turn.YieldFrameworkCallAsync(
                        async _ =>
                        {
                            state.ExternalStarted.TrySetResult();
                            await state.CompleteExternal.Task.ConfigureAwait(false);
                        },
                        ct)
                    .ConfigureAwait(false);
                state.TerminalContinuation.TrySetResult();
            },
            new YieldBarrierState(
                externalStarted,
                completeExternal,
                terminalContinuation),
            CancellationToken.None).AsTask();
        await externalStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var sealTask = executor.SealRelocationAsync(CancellationToken.None).AsTask();
        Assert.False(sealTask.IsCompleted);

        completeExternal.TrySetResult();
        await terminalContinuation.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var seal = await sealTask.WaitAsync(TimeSpan.FromSeconds(5));
        await operation.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(executor.TryAbortRelocation(seal));
    }

    [Fact]
    public async Task SpotWide_RelocationSeal_StopsNewIngressAfterCurrentTurn()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(
            errorSink,
            ZLinkUserSpotExecutionMode.SpotWide);
        var started = NewSignal();
        var release = NewSignal();
        var current = executor.ExecuteActorAsync(
            "actor-1",
            async static (_, state, _) =>
            {
                state.Started.TrySetResult();
                await state.Release.Task.ConfigureAwait(false);
            },
            new BlockState(started, release),
            CancellationToken.None).AsTask();
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var sealTask = executor.SealRelocationAsync(
            CancellationToken.None).AsTask();
        var rejected = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            executor.ExecuteActorAsync(
                    "actor-2",
                    static (_, _, _) => ValueTask.CompletedTask,
                    0,
                    CancellationToken.None)
                .AsTask());
        Assert.Equal(ZLinkFrameworkErrorKind.Rejected, rejected.Kind);
        Assert.False(sealTask.IsCompleted);

        release.TrySetResult();
        await current.WaitAsync(TimeSpan.FromSeconds(5));
        var seal = await sealTask.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(executor.TryAbortRelocation(seal));
    }

    [Fact]
    public async Task SpotWide_RelocationSeal_ContinuousIngressCannotStarveBoundary()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(
            errorSink,
            ZLinkUserSpotExecutionMode.SpotWide);
        var currentStarted = NewSignal();
        var releaseCurrent = NewSignal();
        var current = executor.ExecuteActorAsync(
            "actor-current",
            async static (_, state, _) =>
            {
                state.Started.TrySetResult();
                await state.Release.Task.ConfigureAwait(false);
            },
            new BlockState(currentStarted, releaseCurrent),
            CancellationToken.None).AsTask();
        await currentStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var sealTask = executor.SealRelocationAsync(
            CancellationToken.None).AsTask();
        var unexpectedExecutions = 0;
        var rejectedAdmissions = 0;
        var producers = Enumerable.Range(0, 4)
            .Select(producer => Task.Run(async () =>
            {
                for (var attempt = 0; attempt < 128; attempt++)
                {
                    try
                    {
                        await executor.ExecuteActorAsync(
                            $"actor-{producer}-{attempt}",
                            (_, _, _) =>
                            {
                                Interlocked.Increment(
                                    ref unexpectedExecutions);
                                return ValueTask.CompletedTask;
                            },
                            0,
                            CancellationToken.None);
                    }
                    catch (ZLinkFrameworkException exception)
                        when (exception.Kind
                              == ZLinkFrameworkErrorKind.Rejected)
                    {
                        Interlocked.Increment(ref rejectedAdmissions);
                    }

                    await Task.Yield();
                }
            }))
            .ToArray();

        releaseCurrent.TrySetResult();
        await current.WaitAsync(TimeSpan.FromSeconds(5));
        var seal = await sealTask.WaitAsync(TimeSpan.FromSeconds(5));
        await Task.WhenAll(producers).WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(0, Volatile.Read(ref unexpectedExecutions));
        Assert.Equal(512, Volatile.Read(ref rejectedAdmissions));
        Assert.True(executor.TryAbortRelocation(seal));
    }

    [Fact]
    public async Task RelocationAbort_OnlyReopensItsOwnBarrierGeneration()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(
            errorSink,
            ZLinkUserSpotExecutionMode.PerActor);

        var first = await executor.SealRelocationAsync(CancellationToken.None);
        Assert.True(executor.TryAbortRelocation(first));
        var second = await executor.SealRelocationAsync(CancellationToken.None);

        Assert.False(executor.TryAbortRelocation(first));
        Assert.True(executor.TryAbortRelocation(second));

        var ran = NewSignal();
        await RecordActor(executor, "actor-1", null, null, ran)
            .WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(ran.Task.IsCompleted);
    }

    [Fact]
    public async Task RelocationReplay_RequiresCurrentExactSeal()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(
            errorSink,
            ZLinkUserSpotExecutionMode.SpotWide);

        var stale = await executor.SealRelocationAsync(
            CancellationToken.None);
        Assert.True(executor.TryAbortRelocation(stale));
        var current = await executor.SealRelocationAsync(
            CancellationToken.None);

        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            executor.ExecuteRelocationActorAsync(
                    stale,
                    "actor-1",
                    static (_, _, _) => ValueTask.CompletedTask,
                    0,
                    CancellationToken.None)
                .AsTask());

        var ran = NewSignal();
        await executor.ExecuteRelocationActorAsync(
                current,
                "actor-1",
                static (_, completion, _) =>
                {
                    completion.TrySetResult();
                    return ValueTask.CompletedTask;
                },
                ran,
                CancellationToken.None)
            .AsTask()
            .WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(ran.Task.IsCompleted);
        Assert.True(executor.TryAbortRelocation(current));
    }

    [Fact]
    public async Task RelocationAdmissionOpenRetriesItsReservationCallback()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(
            errorSink,
            ZLinkUserSpotExecutionMode.SpotWide);
        var seal = await executor.SealRelocationAsync(
            CancellationToken.None);
        var attempts = 0;

        Assert.Throws<InvalidOperationException>(() =>
            executor.TryOpenRelocationAfterMessageFollow(
                seal,
                () =>
                {
                    attempts++;
                    throw new InvalidOperationException("retry");
                }));
        Assert.True(executor.TryOpenRelocationAfterMessageFollow(
            seal,
            () => attempts++));

        Assert.Equal(2, attempts);
    }

    [Fact]
    public async Task SpotWideRelocationReplay_ReservesSharedQueueBeforeDirectIngress()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(
            errorSink,
            ZLinkUserSpotExecutionMode.SpotWide);
        var seal = await executor.SealRelocationAsync(
            CancellationToken.None);
        var order = new ConcurrentQueue<string>();
        var route = new ZLinkBackendActorRouteContext(
            new MeshOperationId(
                0x1122334455667788,
                0x99aabbccddeeff00),
            MessageFollowHopCount: 1,
            TargetNodeGeneration: 7,
            AuthorityOwnerGeneration: 11,
            OwnerLeaseGeneration: 13,
            ReplyRequestId: 17,
            ReplyFlags: 19,
            ReplyCapability: "reply-capability",
            DeadlineUnixMs: 23);
        var probe = new ReplayContractProbe(route, new object());
        var migrated = executor.ReserveRelocationActorQueue(
            seal,
            "actor-1");
        var targetCaptured = executor.ReserveRelocationActorQueue(
            seal,
            "actor-1");

        // Simulate authority publication. Message Follow and fresh direct
        // ingress can now submit, but both must remain behind the replay
        // positions reserved while the target admission seal was active.
        Assert.True(executor.TryAbortRelocation(seal));
        var sourceFollow = RecordActor(
            executor,
            "actor-1",
            order,
            "source-follow",
            NewSignal());
        var direct = RecordActor(
            executor,
            "actor-1",
            order,
            "direct",
            NewSignal());

        var migratedExecution = migrated.ExecuteAsync(
            "actor-1",
            _ =>
            {
                Assert.Equal(
                    new MeshOperationId(
                        0x1122334455667788,
                        0x99aabbccddeeff00),
                    probe.Route.OperationId);
                Assert.Equal((ulong)23, probe.Route.DeadlineUnixMs);
                Assert.Equal((ulong)17, probe.Route.ReplyRequestId);
                Assert.Equal(
                    "reply-capability",
                    probe.Route.ReplyCapability);
                Assert.NotNull(probe.Ownership);
                order.Enqueue("migrated");
                return ValueTask.CompletedTask;
            },
            CancellationToken.None).AsTask();
        var targetExecution = targetCaptured.ExecuteAsync(
            "actor-1",
            _ =>
            {
                order.Enqueue("target-captured");
                return ValueTask.CompletedTask;
            },
            CancellationToken.None).AsTask();

        await Task.WhenAll(
                migratedExecution,
                targetExecution,
                sourceFollow,
                direct)
            .WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(
            new[]
            {
                "migrated",
                "target-captured",
                "source-follow",
                "direct"
            },
            order.ToArray());
    }

    [Fact]
    public async Task CallerCancellation_DoesNotReleaseRelocationClaimBeforeCallbackEnds()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(
            errorSink,
            ZLinkUserSpotExecutionMode.PerActor);
        using var callerCancellation = new CancellationTokenSource();
        var started = NewSignal();
        var release = NewSignal();

        var operation = executor.ExecuteActorAsync(
            "actor-1",
            async static (_, state, _) =>
            {
                state.Started.TrySetResult();
                await state.Release.Task.ConfigureAwait(false);
            },
            new BlockState(started, release),
            callerCancellation.Token).AsTask();
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));

        callerCancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await operation.ConfigureAwait(false));
        var sealTask = executor.SealRelocationAsync(CancellationToken.None).AsTask();
        Assert.False(sealTask.IsCompleted);

        release.TrySetResult();
        var seal = await sealTask.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(executor.TryAbortRelocation(seal));
    }

    [Fact]
    public async Task ClosingBarrier_WaitsForAllLanesAndKeepsAdmissionSealed()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(
            errorSink,
            ZLinkUserSpotExecutionMode.PerActor);
        var started = NewSignal();
        var release = NewSignal();
        var closingRan = NewSignal();
        var actor = executor.ExecuteActorAsync(
            "actor-1",
            async static (_, state, _) =>
            {
                state.Started.TrySetResult();
                await state.Release.Task.ConfigureAwait(false);
            },
            new BlockState(started, release),
            CancellationToken.None).AsTask();
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var closing = executor.ExecuteQuiescentLifecycleAsync(
            (_, _) =>
            {
                closingRan.TrySetResult();
                return ValueTask.FromResult(true);
            },
            CancellationToken.None).AsTask();
        Assert.False(closingRan.Task.IsCompleted);

        release.TrySetResult();
        await actor.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(await closing.WaitAsync(TimeSpan.FromSeconds(5)));
        Assert.True(closingRan.Task.IsCompleted);

        var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            executor.ExecuteActorAsync(
                    "actor-2",
                    static (_, _, _) => ValueTask.CompletedTask,
                    0,
                    CancellationToken.None)
                .AsTask());
        Assert.Equal(ZLinkFrameworkErrorKind.Rejected, failure.Kind);
    }

    [Fact]
    public async Task ClosingBarrier_WaitsForYieldedAcceptedSpotWork()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(
            errorSink,
            ZLinkUserSpotExecutionMode.SpotWide);
        var externalStarted = NewSignal();
        var releaseExternal = NewSignal();
        var terminal = NewSignal();
        var closingRan = NewSignal();

        Assert.Equal(ZLinkAcceptedWorkAdmission.Accepted, executor.QueueAccepted(
            new byte[] { 1 },
            async (_, ct) =>
            {
                var turn = ZLinkApplicationExecutionContext
                    .RequireYieldTurn("accepted close barrier test");
                await turn.YieldFrameworkCallAsync(
                        async _ =>
                        {
                            externalStarted.TrySetResult();
                            await releaseExternal.Task.ConfigureAwait(false);
                        },
                        ct)
                    .ConfigureAwait(false);
                terminal.TrySetResult();
            },
            static () => { },
            out var acceptedCompletion));
        await externalStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var closing = executor.ExecuteQuiescentLifecycleAsync(
            (_, _) =>
            {
                closingRan.TrySetResult();
                return ValueTask.FromResult(true);
            },
            CancellationToken.None).AsTask();
        Assert.False(closingRan.Task.IsCompleted);

        releaseExternal.TrySetResult();
        await terminal.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await acceptedCompletion.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(await closing.WaitAsync(TimeSpan.FromSeconds(5)));
        Assert.True(closingRan.Task.IsCompleted);
    }

    [Fact]
    public async Task PerActor_Shell_Seal_Does_Not_Wait_For_Actor_Lane()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(
            errorSink,
            ZLinkUserSpotExecutionMode.PerActor);
        var actorStarted = NewSignal();
        var releaseActor = NewSignal();
        var actor = executor.ExecuteActorAsync(
                "actor-a",
                async (_, _, _) =>
                {
                    actorStarted.TrySetResult();
                    await releaseActor.Task.ConfigureAwait(false);
                },
                state: 0,
                CancellationToken.None)
            .AsTask();
        await actorStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.True(executor.TrySealPerActorShellRelocation(out var seal));
        var nextActorRan = NewSignal();
        var nextActor = RecordActor(
            executor,
            "actor-b",
            order: null,
            marker: null,
            nextActorRan);
        await nextActorRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(executor.Queue(
            static (_, _) => ValueTask.CompletedTask));

        Assert.True(executor.TryAbortRelocation(seal));
        releaseActor.TrySetResult();
        await Task.WhenAll(actor, nextActor);
    }

    [Fact]
    public async Task PerActor_Shell_Commit_Keeps_Existing_Actor_Lanes_Runnable()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(
            errorSink,
            ZLinkUserSpotExecutionMode.PerActor);
        var firstActorStarted = NewSignal();
        var releaseFirstActor = NewSignal();
        var firstActor = executor.ExecuteActorAsync(
                "actor-a",
                async (_, _, _) =>
                {
                    firstActorStarted.TrySetResult();
                    await releaseFirstActor.Task.ConfigureAwait(false);
                },
                state: 0,
                CancellationToken.None)
            .AsTask();
        await firstActorStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.True(executor.TrySealPerActorShellRelocation(out var seal));
        Assert.True(executor.TryCommitRelocation(
            seal,
            out var held,
            preserveActorExecution: true));
        Assert.Empty(held);

        var nextActorRan = NewSignal();
        var nextActor = RecordActor(
            executor,
            "actor-a",
            order: null,
            marker: null,
            nextActorRan);
        Assert.False(nextActorRan.Task.IsCompleted);
        Assert.False(executor.Queue(
            static (_, _) => ValueTask.CompletedTask));

        releaseFirstActor.TrySetResult();
        await Task.WhenAll(firstActor, nextActor)
            .WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(nextActorRan.Task.IsCompleted);
    }

    private static ZLinkSpotSerialExecutor CreateExecutor(
        IZLinkRuntimeFailureReporter errorSink,
        ZLinkUserSpotExecutionMode mode)
    {
        return new ZLinkSpotSerialExecutor(
            null!,
            static () => false,
            CancellationToken.None,
            errorSink,
            executionMode: mode);
    }

    private static Task RecordActor(
        ZLinkSpotSerialExecutor executor,
        string actorId,
        ConcurrentQueue<string>? order,
        string? marker,
        TaskCompletionSource signal)
    {
        return executor.ExecuteActorAsync(
            actorId,
            static (_, state, _) =>
            {
                if (state.Order is not null && state.Marker is not null)
                    state.Order.Enqueue(state.Marker);
                state.Signal.TrySetResult();
                return ValueTask.CompletedTask;
            },
            new RecordState(order, marker, signal),
            CancellationToken.None).AsTask();
    }

    private static Task RecordTimer(
        ZLinkSpotSerialExecutor executor,
        string timerName,
        TaskCompletionSource signal)
    {
        return executor.ExecuteTimerAsync(
            timerName,
            static (_, completion, _) =>
            {
                completion.TrySetResult();
                return ValueTask.CompletedTask;
            },
            signal,
            CancellationToken.None).AsTask();
    }

    private static void AssertInvalid(Action operation)
    {
        var failure = Assert.Throws<ZLinkFrameworkException>(operation);
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, failure.Kind);
    }

    private static TaskCompletionSource NewSignal() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private sealed record YieldState(
        ConcurrentQueue<string> Order,
        TaskCompletionSource ExternalStarted,
        TaskCompletionSource CompleteExternal,
        TaskCompletionSource FirstResumed);

    private sealed record RecordState(
        ConcurrentQueue<string>? Order,
        string? Marker,
        TaskCompletionSource Signal);

    private sealed record BlockState(
        TaskCompletionSource Started,
        TaskCompletionSource Release);

    private sealed record ReplayContractProbe(
        ZLinkBackendActorRouteContext Route,
        object Ownership);

    private sealed record YieldBarrierState(
        TaskCompletionSource ExternalStarted,
        TaskCompletionSource CompleteExternal,
        TaskCompletionSource TerminalContinuation);

    private sealed class ExecutionTestSpot(IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;
    }

    private sealed class OtherExecutionTestSpot(IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;
    }
}
