using System.Collections.Concurrent;
using Systems.Zlink.Stream.Connector.Contracts;

namespace Zlink.Framework.UnitTests;

public sealed class SerialExecutorTests
{
    [Fact]
    public async Task CanceledActorAdmissionReturnsCanceledTaskAndAllowsLaterWork()
    {
        using var errors = new ZLinkRuntimeErrorSink();
        await using var executor = new ZLinkSpotSerialExecutor(
            null!,
            static () => false,
            CancellationToken.None,
            errors
        );
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        var calls = new int[1];
        static ValueTask Operation(ZLinkSpotActivation _, int[] calls, CancellationToken token)
        {
            Interlocked.Increment(ref calls[0]);
            return ValueTask.CompletedTask;
        }
        var canceled = executor.ExecuteActorAsync(
            "canceled-actor",
            Operation,
            calls,
            cancellation.Token
        );
        Assert.True(canceled.IsCanceled);
        var error = await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            canceled.AsTask()
        );
        Assert.Equal(cancellation.Token, error.CancellationToken);
        Assert.Equal(0, calls[0]);
        await executor.ExecuteActorAsync(
            "canceled-actor",
            Operation,
            calls,
            CancellationToken.None
        );
        Assert.Equal(1, calls[0]);
    }

    [Fact]
    public async Task QueuedActorExecutionAllocatesOnlyAdmissionAndClaimStorage()
    {
        const int count = 1000;
        using var errors = new ZLinkRuntimeErrorSink();
        using var entered = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var selected = 0;
        await using var executor = new ZLinkSpotSerialExecutor(
            null!,
            static () => false,
            CancellationToken.None,
            errors,
            actorConsumerSelected: () =>
            {
                if (Interlocked.Exchange(ref selected, 1) == 0)
                {
                    entered.Set();
                    release.Wait();
                }
            }
        );
        await using var baselineLane = new ZLinkStateLane();
        object state = new int[1];
        Func<ZLinkSpotActivation, object, CancellationToken, ValueTask> operation = static (
            _,
            state,
            _
        ) =>
        {
            Interlocked.Increment(ref ((int[])state)[0]);
            return ValueTask.CompletedTask;
        };
        Func<CancellationToken, ValueTask> callback = static _ => ValueTask.CompletedTask;
        Func<bool> stateCallback = static () => false;
        var pending = new Task[2];
        var windowTasks = new List<Task[]>();
        pending[0] = executor
            .ExecuteActorAsync("actor-cost", operation, state, CancellationToken.None)
            .AsTask();
        try
        {
            entered.Wait();
            pending[1] = executor
                .ExecuteActorAsync("actor-cost", operation, state, CancellationToken.None)
                .AsTask();
            void Baseline()
            {
                baselineLane
                    .RunAsync(
                        static work => ZLinkRuntimeTaskRunner.WithoutExecutionContextFlow(work),
                        stateCallback
                    )
                    .GetAwaiter()
                    .GetResult();
                _ = executor
                    .ActorIngressOwner!.PostAsync(callback, CancellationToken.None)
                    .GetAwaiter()
                    .GetResult();
                // Child publication adds a mailbox marker that parent admission does not create.
                GC.KeepAlive(new ZLinkSerialWorkItem(executor.ActorIngressOwner));
                // Reference slots model the two callback captures and the claim storage.
                GC.KeepAlive(
                    Tuple.Create<object, object, object, object>(state, state, state, state)
                );
                GC.KeepAlive(Tuple.Create<object, object, object>(state, state, state));
                GC.KeepAlive(Tuple.Create<object, bool, int>(state, true, 0));
                GC.KeepAlive(new Func<CancellationToken, ValueTask>(callback.Invoke));
                GC.KeepAlive(new Func<CancellationToken, ValueTask>(callback.Invoke));
            }
            Baseline();
            var admissionStorage = PerCallAllocation.MinWindow(Baseline, count);
            Task[] current = [];
            var actual = PerCallAllocation.MinWindow(
                i =>
                    current[i] = executor
                        .ExecuteActorAsync("actor-cost", operation, state, CancellationToken.None)
                        .AsTask(),
                count,
                beforeWindow: _ => windowTasks.Add(current = new Task[count])
            );
            Assert.Equal(admissionStorage, actual);
        }
        finally
        {
            release.Set();
            await Task.WhenAll(
                pending
                    .Concat(windowTasks.SelectMany(static window => window))
                    .Where(static task => task is not null)
            );
        }
        Assert.Equal(count * windowTasks.Count + 2, ((int[])state)[0]);
    }

    [Fact]
    public async Task RelocationSnapshotsShareTheAdmissionOwnedPayloadMaterialization()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var seal = await queue.TrySealRelocationAsync();
        Assert.NotNull(seal);
        var materialized = 0;
        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            queue.TryPostAccepted(
                1,
                () =>
                {
                    Interlocked.Increment(ref materialized);
                    return new byte[] { 7 };
                },
                static _ => ValueTask.CompletedTask,
                static () => { },
                false,
                out _
            )
        );
        var snapshots = await Task.WhenAll(
            queue.TryFreezeRelocationIngressAsync(seal).AsTask(),
            queue.TryFreezeRelocationIngressAsync(seal).AsTask()
        );
        Assert.All(snapshots, snapshot => Assert.True(snapshot.Item1));
        Assert.Equal(1, materialized);
        var (committed, held) = await queue.TryCommitRelocationAsync(seal);
        Assert.True(committed);
        Assert.Equal(new byte[] { 7 }, Assert.Single(held).Payload.ToArray());
        Assert.Equal(1, materialized);
    }

    [Fact]
    public void AcceptedPayloadFactoryAddsNoSeparateSynchronizationObject()
    {
        const int count = 1000;
        Func<CancellationToken, ValueTask> callback = static _ => ValueTask.CompletedTask;
        Func<ReadOnlyMemory<byte>> factory = static () => ReadOnlyMemory<byte>.Empty;
        _ = new ZLinkSerialWorkItem(callback, acceptedPayloadFactory: factory);
        var baseline = PerCallAllocation.MinWindow(
            () => GC.KeepAlive(new ZLinkSerialWorkItem(callback)),
            count
        );
        var actual = PerCallAllocation.MinWindow(
            () => GC.KeepAlive(new ZLinkSerialWorkItem(callback, acceptedPayloadFactory: factory)),
            count
        );
        Assert.Equal(baseline, actual);
    }

    [Fact]
    public void ApplicationScopeAllocatesOnlyItsClaimAndAmbientCarrier()
    {
        Exception? failure = null;
        var thread = new Thread(() =>
        {
            try
            {
                var ambient = new AsyncLocal<ZLinkApplicationExecutionScope?>();
                var input = new ZLinkApplicationExecutionScope(
                    "scope-cost",
                    ZLinkUserSpotExecutionMode.SpotWide,
                    null,
                    true
                );
                void Baseline()
                {
                    var claim = new ZLinkApplicationExecutionClaim();
                    ambient.Value = input with { Claim = claim };
                    claim.Deactivate();
                    ambient.Value = null;
                }
                void Actual()
                {
                    using var scope = ZLinkApplicationExecutionContext.Push(input);
                }
                for (var i = 0; i < 1000; i++)
                {
                    Baseline();
                    Actual();
                }
                var ambientStorage = PerCallAllocation.MinWindow(Baseline, 1000);
                Assert.Equal(ambientStorage, PerCallAllocation.MinWindow(Actual, 1000));
            }
            catch (Exception exception)
            {
                failure = exception;
            }
        });
        using (ExecutionContext.SuppressFlow())
            thread.Start();
        thread.Join();
        if (failure is not null)
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failure).Throw();
    }

    [Fact]
    public async Task ApplicationScopeRestoresOuterScopeAndDeactivatesCapturedClaims()
    {
        var input = new ZLinkApplicationExecutionScope(
            "outer",
            ZLinkUserSpotExecutionMode.SpotWide,
            null,
            true
        );
        var outer = ZLinkApplicationExecutionContext.Push(input);
        var outerClaim = ZLinkApplicationExecutionContext.Current!.Value.Claim!;
        ZLinkApplicationExecutionClaim innerClaim;
        try
        {
            using (ZLinkApplicationExecutionContext.Push(input with { SpotId = "inner" }))
            {
                innerClaim = ZLinkApplicationExecutionContext.Current!.Value.Claim!;
                await Task.Yield();
                Assert.Equal("inner", ZLinkApplicationExecutionContext.Current!.Value.SpotId);
                Assert.True(innerClaim.IsActive);
            }
            Assert.False(innerClaim.IsActive);
            Assert.Equal("outer", ZLinkApplicationExecutionContext.Current!.Value.SpotId);
            Assert.Same(outerClaim, ZLinkApplicationExecutionContext.Current!.Value.Claim);
        }
        finally
        {
            outer.Dispose();
        }
        Assert.False(outerClaim.IsActive);
        Assert.Null(ZLinkApplicationExecutionContext.Current);
    }

    [Fact]
    public async Task SpotStateReadAllocatesOnlyItsCallbackAndStateLaneContext()
    {
        using var errors = new ZLinkRuntimeErrorSink();
        await using var executor = new ZLinkSpotSerialExecutor(
            null!,
            static () => false,
            CancellationToken.None,
            errors
        );
        var baseline = new ZLinkStateLane();
        Func<bool> callback = static () => false;
        void Baseline()
        {
            baseline
                .RunAsync(
                    static callback => ZLinkRuntimeTaskRunner.WithoutExecutionContextFlow(callback),
                    callback
                )
                .GetAwaiter()
                .GetResult();
        }
        Baseline();
        Assert.False(executor.HasRelocationBarrier);
        var expected = PerCallAllocation.MinWindow(Baseline, 1000);
        var hasBarrier = false;
        var actual = PerCallAllocation.MinWindow(
            () => hasBarrier |= executor.HasRelocationBarrier,
            1000
        );
        Assert.False(hasBarrier);
        Assert.Equal(expected, actual);
    }

    [Fact]
    public async Task AbsentDrainDoesNotAllocate()
    {
        using var errors = new ZLinkRuntimeErrorSink();
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errors, CancellationToken.None),
            errors,
            CancellationToken.None
        );
        queue.PublishDrain(null);
        Assert.Equal(0, PerCallAllocation.MinWindow(() => queue.PublishDrain(null), 1000));
    }

    [Fact]
    public async Task IdleQueueReadinessDoesNotAllocate()
    {
        using var errors = new ZLinkRuntimeErrorSink();
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errors, CancellationToken.None),
            errors,
            CancellationToken.None
        );
        queue.SchedulePendingWork();
        Assert.Equal(0, PerCallAllocation.MinWindow(queue.SchedulePendingWork, 1000));
    }

    [Fact]
    public void ChannelReplyGateAllocatesOnlyItsStateLaneTurns()
    {
        var gate = new ZLinkChannelReplyGate();
        var baseline = new ZLinkStateLane();
        Action reply = static () => { };
        gate.TryInvoke(reply);
        void Baseline()
        {
            baseline.RunAsync(static value => value, true).GetAwaiter().GetResult();
            baseline.RunAsync(static value => value, true).GetAwaiter().GetResult();
        }
        Baseline();
        var laneStorage = PerCallAllocation.MinWindow(Baseline, 1000);
        Assert.Equal(laneStorage, PerCallAllocation.MinWindow(() => gate.TryInvoke(reply), 1000));
    }

    [Fact]
    public async Task BusyQueueAdmissionAllocatesOnlyWorkItemStorage()
    {
        using var errors = new ZLinkRuntimeErrorSink();
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errors, CancellationToken.None),
            errors,
            CancellationToken.None
        );
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var execution = queue
            .RunAsync(
                async _ =>
                {
                    entered.TrySetResult();
                    await release.Task;
                },
                CancellationToken.None
            )
            .AsTask();
        Func<CancellationToken, ValueTask> callback = static _ => ValueTask.CompletedTask;
        try
        {
            await entered.Task;
            Assert.True(queue.TryPost(callback, out _));
            GC.KeepAlive(new ZLinkSerialWorkItem(callback));
            var storage = PerCallAllocation.MinWindow(
                () => GC.KeepAlive(new ZLinkSerialWorkItem(callback)),
                1000
            );
            var admitted = 0;
            var admissionStorage = PerCallAllocation.MinWindow(
                call =>
                {
                    if (queue.TryPost(callback, out _))
                        admitted++;
                },
                1000,
                beforeWindow: _ => admitted = 0
            );
            Assert.Equal(1000, admitted);
            Assert.Equal(storage, admissionStorage);
        }
        finally
        {
            release.TrySetResult();
            await execution;
            await queue.ApplicationDrained;
        }
    }

    [Fact]
    public void SerialTurnConcurrentSuspensionObservationDoesNotLoseSignals()
    {
        const int iterations = 1000;
        var item = new ZLinkSerialWorkItem(static _ => ValueTask.CompletedTask);
        var turn = new ZLinkSerialTurn(
            static (_, _) => ZLinkSerialPostAdmission.Closed,
            static _ => false,
            static _ => { },
            CancellationToken.None,
            item
        );
        using var phases = new Barrier(2);
        var signaler = new Thread(() =>
        {
            for (var i = 0; i < iterations; i++)
            {
                phases.SignalAndWait();
                turn.SignalSuspended();
                phases.SignalAndWait();
            }
        });
        signaler.Start();
        var completed = 0;
        try
        {
            for (var i = 0; i < iterations; i++)
            {
                turn.ResetSuspension();
                phases.SignalAndWait();
                var signal = turn.Suspended;
                phases.SignalAndWait();
                if (signal.IsCompletedSuccessfully)
                    completed++;
            }
        }
        finally
        {
            signaler.Join();
        }
        Assert.Equal(iterations, completed);
    }

    [Fact]
    public void SerialTurnConcurrentObserversShareTheSuspensionSignal()
    {
        const int iterations = 1000;
        var item = new ZLinkSerialWorkItem(static _ => ValueTask.CompletedTask);
        var turn = new ZLinkSerialTurn(
            static (_, _) => ZLinkSerialPostAdmission.Closed,
            static _ => false,
            static _ => { },
            CancellationToken.None,
            item
        );
        using var phases = new Barrier(4);
        var observed = new Task?[2];
        var workers = new Thread[3];
        for (var worker = 0; worker < workers.Length; worker++)
        {
            var index = worker;
            workers[worker] = new Thread(() =>
            {
                for (var i = 0; i < iterations; i++)
                {
                    phases.SignalAndWait();
                    if (index < observed.Length)
                        observed[index] = turn.Suspended;
                    else
                        turn.SignalSuspended();
                    phases.SignalAndWait();
                }
            });
            workers[worker].Start();
        }
        var completed = 0;
        var shared = 0;
        try
        {
            for (var i = 0; i < iterations; i++)
            {
                turn.ResetSuspension();
                phases.SignalAndWait();
                phases.SignalAndWait();
                if (observed[0]!.IsCompletedSuccessfully && observed[1]!.IsCompletedSuccessfully)
                    completed++;
                if (ReferenceEquals(observed[0], observed[1]))
                    shared++;
            }
        }
        finally
        {
            foreach (var worker in workers)
                worker.Join();
        }
        Assert.Equal(iterations, completed);
        Assert.Equal(iterations, shared);
    }

    [Fact]
    public async Task ApplicationDrainWaitersAreIsolatedAcrossPendingGenerations()
    {
        using var errors = new ZLinkRuntimeErrorSink();
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errors, CancellationToken.None),
            errors,
            CancellationToken.None
        );
        Task? previous = null;
        for (var cycle = 0; cycle < 2; cycle++)
        {
            var entered = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            var release = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            var execution = queue
                .RunAsync(
                    async _ =>
                    {
                        entered.TrySetResult();
                        await release.Task;
                    },
                    CancellationToken.None
                )
                .AsTask();
            try
            {
                await entered.Task;
                var drained = queue.ApplicationDrained;
                Assert.False(drained.IsCompleted);
                Assert.Same(drained, queue.ApplicationDrained);
                if (previous is not null)
                {
                    Assert.NotSame(previous, drained);
                    Assert.True(previous.IsCompletedSuccessfully);
                }
                release.TrySetResult();
                await execution;
                await drained;
                previous = drained;
            }
            finally
            {
                release.TrySetResult();
                await execution;
            }
        }
    }

    [Fact]
    public async Task TerminalCompositionAllocatesOnlyItsTwoCallbackReferences()
    {
        using var errors = new ZLinkRuntimeErrorSink();
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errors, CancellationToken.None),
            errors,
            CancellationToken.None
        );
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirst = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var execution = queue
            .RunAsync(
                async _ =>
                {
                    entered.TrySetResult();
                    await releaseFirst.Task;
                },
                CancellationToken.None
            )
            .AsTask();
        var items = new ZLinkSerialWorkItem[1000];
        Action release = static () => { };
        static Action Baseline(object current, Action release) =>
            () =>
            {
                GC.KeepAlive(current);
                release();
            };
        try
        {
            await entered.Task;
            GC.KeepAlive(Baseline(queue, release));
            var stateStorage = PerCallAllocation.MinWindow(
                () => GC.KeepAlive(Baseline(queue, release)),
                items.Length
            );
            // Every window releases onto freshly posted items, posted outside the window.
            var releaseStorage = PerCallAllocation.MinWindow(
                i => items[i].AddTerminalRelease(release),
                items.Length,
                beforeWindow: _ =>
                {
                    for (var i = 0; i < items.Length; i++)
                        Assert.True(
                            queue.TryPost(static _ => ValueTask.CompletedTask, out items[i])
                        );
                }
            );
            Assert.Equal(stateStorage, releaseStorage);
        }
        finally
        {
            releaseFirst.TrySetResult();
            await execution;
            await queue.ApplicationDrained;
        }
    }

    [Fact]
    public async Task ComposedTerminalFailureStillReleasesQueueReservationExactlyOnce()
    {
        using var errors = new ZLinkRuntimeErrorSink();
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errors, CancellationToken.None),
            errors,
            CancellationToken.None
        );
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var execution = queue
            .RunAsync(
                async _ =>
                {
                    entered.TrySetResult();
                    await release.Task;
                },
                CancellationToken.None
            )
            .AsTask();
        try
        {
            await entered.Task;
            Assert.True(queue.TryPost(static _ => ValueTask.CompletedTask, out var item));
            var releases = 0;
            item.AddTerminalRelease(() =>
            {
                releases++;
                throw new InvalidOperationException("terminal failure");
            });
            release.TrySetResult();
            await execution;
            await Assert.ThrowsAsync<InvalidOperationException>(() => item.Completion);
            await queue.ApplicationDrained;
            item.ReleaseForRelocation(static _ => { });
            Assert.Equal(1, releases);
            Assert.Equal(0, queue.ApplicationPendingCount);
        }
        finally
        {
            release.TrySetResult();
            await execution;
        }
    }

    [Fact]
    public async Task ApplicationDrainSignalIsAllocatedOnlyWhenObserved()
    {
        const int windowQueues = 32;
        var signalStorage = PerCallAllocation.MinWindow(
            () =>
                GC.KeepAlive(
                    new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously)
                ),
            windowQueues
        );
        // The signal is created by the first observation of a busy queue, so each
        // sample observes its own fresh queue; a window is the samples of 32 queues.
        var observedSamples = new List<long>();
        for (var round = 0; round < windowQueues * PerCallAllocation.Windows; round++)
        {
            using var errors = new ZLinkRuntimeErrorSink();
            await using var queue = new ZLinkSerialExecutionQueue(
                new ZLinkRuntimeTaskRunner(errors, CancellationToken.None),
                errors,
                CancellationToken.None
            );
            Assert.True(queue.ApplicationDrained.IsCompletedSuccessfully);
            var entered = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            var release = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            var execution = queue
                .RunAsync(
                    async _ =>
                    {
                        entered.TrySetResult();
                        await release.Task;
                    },
                    CancellationToken.None
                )
                .AsTask();
            try
            {
                await entered.Task;
                var pause = GC.GetTotalPauseDuration();
                var before = GC.GetAllocatedBytesForCurrentThread();
                var drained = queue.ApplicationDrained;
                var observedStorage = GC.GetAllocatedBytesForCurrentThread() - before;
                if (GC.GetTotalPauseDuration() == pause)
                    observedSamples.Add(observedStorage);
                Assert.Same(drained, queue.ApplicationDrained);
                Assert.False(drained.IsCompleted);
                release.TrySetResult();
                await execution;
                await drained;
                Assert.True(queue.ApplicationDrained.IsCompletedSuccessfully);
            }
            finally
            {
                release.TrySetResult();
                await execution;
            }
        }
        Assert.Equal(signalStorage, PerCallAllocation.MinWindow(observedSamples));
    }

    [Fact]
    public void SerialTurnSuspensionSignalPreservesObservationAndReset()
    {
        var item = new ZLinkSerialWorkItem(static _ => ValueTask.CompletedTask);
        var turn = new ZLinkSerialTurn(
            static (_, _) => ZLinkSerialPostAdmission.Closed,
            static _ => false,
            static _ => { },
            CancellationToken.None,
            item
        );
        var first = turn.Suspended;
        Assert.Same(first, turn.Suspended);
        Assert.False(first.IsCompleted);
        turn.SignalSuspended();
        Assert.True(first.IsCompletedSuccessfully);
        turn.SignalSuspended();
        Assert.Same(first, turn.Suspended);
        turn.ResetSuspension();
        turn.SignalSuspended();
        var second = turn.Suspended;
        Assert.NotSame(first, second);
        Assert.True(second.IsCompletedSuccessfully);
        turn.ResetSuspension();
        var third = turn.Suspended;
        Assert.False(third.IsCompleted);
        turn.SignalSuspended();
        Assert.True(third.IsCompletedSuccessfully);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task SerialWorkItem_CancellationStillObservesCallbackSuspension(bool lifecycle)
    {
        var timeout = TimeSpan.FromSeconds(5);
        using var cancellation = new CancellationTokenSource();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var yield = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var errors = new ZLinkRuntimeErrorSink();
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errors, CancellationToken.None),
            errors,
            CancellationToken.None
        );
        var item = new ZLinkSerialWorkItem(
            async _ =>
            {
                entered.TrySetResult();
                await yield.Task;
                ZLinkSerialTurn.Current!.SignalSuspended();
                await release.Task;
            },
            lane: lifecycle ? ZLinkSerialWorkLane.Lifecycle : ZLinkSerialWorkLane.Application,
            reservationHeld: false
        );
        item.BindTerminalRelease(queue);
        var turn = new ZLinkSerialTurn(
            static (_, _) => ZLinkSerialPostAdmission.Closed,
            static _ => false,
            static _ => { },
            cancellation.Token,
            item
        );
        var invocation = item.InvokeAsync(static _ => { }, cancellation.Token, turn).AsTask();
        try
        {
            await entered.Task.WaitAsync(timeout);
            cancellation.Cancel();
            yield.TrySetResult();
            Assert.Equal(ZLinkSerialWorkItemResult.Suspended, await invocation.WaitAsync(timeout));
            Assert.False(item.Completion.IsCompleted);
        }
        finally
        {
            release.TrySetResult();
            await invocation.WaitAsync(timeout);
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                item.Completion.WaitAsync(timeout)
            );
        }
    }

    [Fact]
    public async Task SerialExecutionQueue_BusyActorDoesNotStartDurablePreparation()
    {
        using var errors = new ZLinkRuntimeErrorSink();
        var runner = new ZLinkRuntimeTaskRunner(errors, CancellationToken.None);
        await using var owner = new ZLinkSerialExecutionQueue(
            runner,
            errors,
            CancellationToken.None,
            ZLinkExecutionLanePolicy.Default,
            sharedGate: true
        );
        await using var mailbox = new ZLinkSerialExecutionQueue(
            runner,
            errors,
            CancellationToken.None,
            ZLinkExecutionLanePolicy.Default,
            sharedOwner: owner
        );
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var external = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var prepared = false;
        var first = mailbox
            .RunAsync(
                async _ =>
                {
                    entered.TrySetResult();
                    await ZLinkSerialTurn.Current!.YieldFrameworkCallAsync(
                        _ => new ValueTask(external.Task),
                        CancellationToken.None
                    );
                },
                CancellationToken.None
            )
            .AsTask();
        try
        {
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(
                ZLinkSerialPostAdmission.Accepted,
                mailbox.TryPostDurableApplication(
                    static _ => ValueTask.CompletedTask,
                    _ =>
                    {
                        prepared = true;
                        return ValueTask.CompletedTask;
                    },
                    static () => { },
                    out var durable
                )
            );
            // Publication is ahead of this owner turn, so its scheduling
            // probe has run while the Actor still retains its FIFO claim.
            await owner
                .RunAsync(static _ => ValueTask.CompletedTask, CancellationToken.None)
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(prepared);
            external.TrySetResult();
            await Task.WhenAll(first, durable.Completion).WaitAsync(TimeSpan.FromSeconds(5));
            Assert.True(prepared);
        }
        finally
        {
            external.TrySetResult();
            await first.WaitAsync(TimeSpan.FromSeconds(5));
        }
    }

    [Fact]
    public async Task SerialExecutionQueue_BusyClaimAllowsMailboxBehindReadyReservedHead()
    {
        using var errors = new ZLinkRuntimeErrorSink();
        var runner = new ZLinkRuntimeTaskRunner(errors, CancellationToken.None);
        await using var owner = new ZLinkSerialExecutionQueue(
            runner,
            errors,
            CancellationToken.None,
            ZLinkExecutionLanePolicy.Default,
            sharedGate: true
        );
        await using var claimed = new ZLinkSerialExecutionQueue(
            runner,
            errors,
            CancellationToken.None,
            ZLinkExecutionLanePolicy.Default,
            sharedOwner: owner
        );
        // Arrange this selector input directly: a retained claim, a ready held
        // head, and a non-reserved mailbox. No consumer runs on the fixture.
        const System.Reflection.BindingFlags flags =
            System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic;
        var type = typeof(ZLinkSerialExecutionQueue);
        var active = type.GetField("_active", flags)!;
        var pending = (ZLinkSerialWorkQueue)
            type.GetField("_applicationQueue", flags)!.GetValue(claimed)!;
        var headReads = 0;
        var head = new ZLinkSerialWorkItem(
            static _ => ValueTask.CompletedTask,
            ready: () =>
            {
                headReads++;
                return true;
            }
        );
        var mailbox = new ZLinkSerialWorkItem(owner);
        active.SetValue(claimed, new ZLinkSerialWorkItem(static _ => ValueTask.CompletedTask));
        pending.Enqueue(head);
        pending.Enqueue(mailbox);
        try
        {
            Assert.True(head.ReservationHeld);
            Assert.False(mailbox.ReservationHeld);
            Assert.NotNull(mailbox.Mailbox);
            Assert.True(
                (bool)
                    type.GetMethod("HasRunnableApplicationUnderLock", flags)!.Invoke(claimed, null)!
            );
            Assert.True(headReads > 0);
            var arguments = new object?[] { null };
            Assert.True(
                (bool)type.GetMethod("TryDequeueNextUnderLock", flags)!.Invoke(claimed, arguments)!
            );
            Assert.Same(mailbox, arguments[0]);
            Assert.Same(head, pending.Head);
        }
        finally
        {
            pending.Clear();
            active.SetValue(claimed, null);
        }
    }

    [Fact]
    public async Task SerialExecutionQueue_ReadinessPublishedDuringSelectionPreservesHeadPosition()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var published = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var order = new ConcurrentQueue<string>();
        var ready = false;
        Assert.True(
            queue.TryPost(
                async _ =>
                {
                    entered.TrySetResult();
                    await release.Task.ConfigureAwait(false);
                },
                out var blocker
            )
        );
        try
        {
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var head = await queue.PostAsync(
                _ =>
                {
                    order.Enqueue("head");
                    return ValueTask.CompletedTask;
                },
                0,
                0,
                false,
                CancellationToken.None,
                ready: () =>
                {
                    var snapshot = Volatile.Read(ref ready);
                    // Publish after this read has rejected the head. Signal the
                    // publisher outside the selector lock to notify the queue.
                    if (!snapshot)
                    {
                        Volatile.Write(ref ready, true);
                        published.TrySetResult();
                    }
                    return snapshot;
                }
            );
            Assert.True(
                queue.TryPost(
                    _ =>
                    {
                        order.Enqueue("following");
                        return ValueTask.CompletedTask;
                    },
                    out var following
                )
            );
            release.TrySetResult();
            await published.Task.WaitAsync(TimeSpan.FromSeconds(5));
            queue.NotifyReadiness();
            await Task.WhenAll(blocker.Completion, head.Completion, following.Completion)
                .WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(new[] { "head", "following" }, order.ToArray());
        }
        finally
        {
            release.TrySetResult();
            Volatile.Write(ref ready, true);
            queue.NotifyReadiness();
        }
    }

    [Fact]
    public async Task RelocationTimerReservationsShareAcceptedSequenceDomain()
    {
        await using var queue = CreateQueue(CancellationToken.None);

        var (succeeded, seal, firstTimerSequence) = await queue.TrySealRelocationAsync(
            reservedAcceptedSequences: 2,
            static _ => true
        );
        Assert.True(succeeded);
        Assert.Equal<ulong>(1, firstTimerSequence);
        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            queue.TryPostAccepted(
                new byte[] { 3 },
                static _ => ValueTask.CompletedTask,
                static () => { },
                out _
            )
        );

        var (heldSucceeded, held) = await queue.TryCommitRelocationAsync(seal);

        Assert.True(heldSucceeded);
        Assert.Equal<ulong>(3, Assert.Single(held).AcceptedSequence);
    }

    [Fact]
    public async Task SerialExecutionQueue_RelocationSeal_CapturesPendingAndHoldsNewAcceptedWork()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var activeStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseActive = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var executionOrder = new ConcurrentQueue<int>();
        var relocated = new ConcurrentQueue<int>();

        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            queue.TryPostAccepted(
                new byte[] { 1 },
                async _ =>
                {
                    activeStarted.TrySetResult();
                    await releaseActive.Task.ConfigureAwait(false);
                    executionOrder.Enqueue(1);
                },
                () => relocated.Enqueue(1),
                out _
            )
        );
        await activeStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            queue.TryPostAccepted(
                new byte[] { 2 },
                _ =>
                {
                    executionOrder.Enqueue(2);
                    return ValueTask.CompletedTask;
                },
                () => relocated.Enqueue(2),
                out _
            )
        );
        var sealTask = queue.SealRelocationAsync(CancellationToken.None).AsTask();
        Assert.False(sealTask.IsCompleted);

        releaseActive.TrySetResult();
        var seal = await sealTask.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Collection(
            seal.Captured,
            record =>
            {
                Assert.Equal<ulong>(2, record.AcceptedSequence);
                Assert.Equal(new byte[] { 2 }, record.Payload.ToArray());
            }
        );

        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            queue.TryPostAccepted(
                new byte[] { 3 },
                _ =>
                {
                    executionOrder.Enqueue(3);
                    return ValueTask.CompletedTask;
                },
                () => relocated.Enqueue(3),
                out _
            )
        );
        Assert.True(await queue.TryAbortRelocationAsync(seal));

        await WaitUntilAsync(() => executionOrder.Count == 3, TimeSpan.FromSeconds(5));
        Assert.Equal(new[] { 1, 2, 3 }, executionOrder);
        Assert.Empty(relocated);
    }

    [Fact]
    public async Task Accepted_journal_factory_materializes_only_after_relocation_seal()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var materialized = 0;

        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            queue.TryPostAccepted(
                new byte[] { 1 },
                callback: async _ =>
                {
                    started.TrySetResult();
                    await release.Task.ConfigureAwait(false);
                },
                relocationRelease: static () => { },
                out _
            )
        );

        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            queue.TryPostAccepted(
                payloadLength: 3,
                payloadFactory: () =>
                {
                    Interlocked.Increment(ref materialized);
                    return new byte[] { 7, 8, 9 };
                },
                callback: static _ => ValueTask.CompletedTask,
                relocationRelease: static () => { },
                previousOwnerMessageFollow: false,
                out _
            )
        );
        Assert.Equal(0, Volatile.Read(ref materialized));

        var sealTask = queue.SealRelocationAsync(CancellationToken.None).AsTask();
        release.TrySetResult();
        var seal = await sealTask.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(1, Volatile.Read(ref materialized));
        Assert.Equal(new byte[] { 7, 8, 9 }, Assert.Single(seal.Captured).Payload.ToArray());
        Assert.True(await queue.TryAbortRelocationAsync(seal));
    }

    [Fact]
    public async Task Cancelled_async_relocation_seal_does_not_orphan_a_later_seal()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var activeStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseActive = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        Assert.True(
            queue.TryPost(
                async _ =>
                {
                    activeStarted.TrySetResult();
                    await releaseActive.Task.ConfigureAwait(false);
                },
                out var active
            )
        );
        await activeStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        using var cancellation = new CancellationTokenSource();
        var cancelledSeal = queue.SealRelocationAsync(cancellation.Token).AsTask();
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => cancelledSeal);

        releaseActive.TrySetResult();
        await active.Completion.WaitAsync(TimeSpan.FromSeconds(5));
        var nextRan = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Assert.True(
            queue.TryPost(
                _ =>
                {
                    nextRan.TrySetResult();
                    return ValueTask.CompletedTask;
                },
                out _
            )
        );
        await nextRan.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var seal = await queue.SealRelocationAsync(CancellationToken.None);
        Assert.True(await queue.TryAbortRelocationAsync(seal));
    }

    [Fact]
    public async Task Async_seal_reserves_timer_sequence_at_boundary_before_held_ingress()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var activeStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseActive = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        Assert.True(
            queue.TryPost(
                async _ =>
                {
                    activeStarted.TrySetResult();
                    await releaseActive.Task.ConfigureAwait(false);
                },
                out _
            )
        );
        await activeStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var boundaryReached = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var sealTask = queue
            .SealRelocationAsync(
                () =>
                {
                    boundaryReached.TrySetResult();
                    return 1;
                },
                CancellationToken.None
            )
            .AsTask();
        releaseActive.TrySetResult();
        await boundaryReached.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var seal = await sealTask.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal<ulong>(1, seal.FirstReservedSequence);
        Assert.Equal(1, seal.ReservedAcceptedSequences);

        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            queue.TryPostAccepted(
                new byte[] { 9 },
                static _ => ValueTask.CompletedTask,
                static () => { },
                out _
            )
        );
        var (heldSucceeded, held) = await queue.TryCommitRelocationAsync(seal);
        Assert.True(heldSucceeded);
        Assert.Equal<ulong>(2, Assert.Single(held).AcceptedSequence);
    }

    [Fact]
    public async Task TargetCutoverRunsPreviousOwnerMessageFollowBeforeDirectIngress()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var executionOrder = new ConcurrentQueue<int>();
        var activeStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseActive = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        ZLinkSerialWorkItem Post(int value, bool followed)
        {
            Assert.Equal(
                ZLinkAcceptedWorkAdmission.Accepted,
                queue.TryPostAccepted(
                    new byte[] { (byte)value },
                    _ =>
                    {
                        executionOrder.Enqueue(value);
                        return ValueTask.CompletedTask;
                    },
                    static () => { },
                    followed,
                    out var item
                )
            );
            return item;
        }

        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            queue.TryPostAccepted(
                new byte[] { 0 },
                async _ =>
                {
                    activeStarted.TrySetResult();
                    await releaseActive.Task.ConfigureAwait(false);
                    executionOrder.Enqueue(0);
                },
                static () => { },
                out var active
            )
        );
        await activeStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        try
        {
            var captured = Post(1, followed: false);
            var sealTask = queue.SealRelocationAsync(CancellationToken.None).AsTask();
            Assert.False(sealTask.IsCompleted);
            releaseActive.TrySetResult();
            var seal = await sealTask.WaitAsync(TimeSpan.FromSeconds(5));

            var directFirst = Post(2, followed: false);
            var followed = Post(3, followed: true);
            var directSecond = Post(4, followed: false);

            Assert.True(await queue.TryOpenRelocationAfterMessageFollowAsync(seal));
            await Task.WhenAll(
                    active.Completion,
                    captured.Completion,
                    directFirst.Completion,
                    followed.Completion,
                    directSecond.Completion
                )
                .WaitAsync(TimeSpan.FromSeconds(5));

            Assert.Equal(new[] { 0, 1, 3, 2, 4 }, executionOrder);
        }
        finally
        {
            releaseActive.TrySetResult();
        }
    }

    [Fact]
    public async Task SerialExecutionQueue_RelocationCommit_ReleasesCapturedAndReturnsHeldRecords()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var seal = await queue.SealRelocationAsync(CancellationToken.None);
        var released = new ConcurrentQueue<int>();

        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            queue.TryPostAccepted(
                new byte[] { 7 },
                static _ => throw new InvalidOperationException("held work must not execute"),
                () => released.Enqueue(7),
                out var heldItem
            )
        );

        var (heldSucceeded, held) = await queue.TryCommitRelocationAsync(seal);

        Assert.True(heldSucceeded);
        await heldItem.Completion.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Collection(
            held,
            record =>
            {
                Assert.Equal<ulong>(1, record.AcceptedSequence);
                Assert.Equal(new byte[] { 7 }, record.Payload.ToArray());
            }
        );
        Assert.Equal(new[] { 7 }, released);
        Assert.Equal(
            ZLinkAcceptedWorkAdmission.RelocationMoving,
            queue.TryPostAccepted(
                new byte[] { 8 },
                static _ => ValueTask.CompletedTask,
                static () => { },
                out _
            )
        );
    }

    [Fact]
    public async Task RelocationIngressHoldDoesNotApplyFormerCountBound()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var seal = await queue.SealRelocationAsync(CancellationToken.None);
        for (var index = 0; index < 1_025; index++)
            Assert.Equal(
                ZLinkAcceptedWorkAdmission.Accepted,
                queue.TryPostAccepted(
                    new byte[] { 7 },
                    static _ => ValueTask.CompletedTask,
                    static () => { },
                    out _
                )
            );

        var (heldSucceeded, held) = await queue.TryCommitRelocationAsync(seal);

        Assert.True(heldSucceeded);
        Assert.Equal(1_025, held.Count);
    }

    [Fact]
    public async Task RelocationIngressHoldDoesNotApplyFormerByteBound()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var seal = await queue.SealRelocationAsync(CancellationToken.None);
        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            queue.TryPostAccepted(
                new byte[16 * 1024 * 1024 - sizeof(ulong) - sizeof(int)],
                static _ => ValueTask.CompletedTask,
                static () => { },
                out _
            )
        );
        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            queue.TryPostAccepted(
                new byte[] { 1 },
                static _ => ValueTask.CompletedTask,
                static () => { },
                out _
            )
        );

        var (heldSucceeded, held) = await queue.TryCommitRelocationAsync(seal);

        Assert.True(heldSucceeded);
        Assert.Equal(2, held.Count);
    }

    [Fact]
    public async Task SerialExecutionQueue_Dispose_AbortsUnfinishedRelocationWithoutHanging()
    {
        var queue = CreateQueue(CancellationToken.None);
        var seal = await queue.SealRelocationAsync(CancellationToken.None);
        var executed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            queue.TryPostAccepted(
                new byte[] { 9 },
                _ =>
                {
                    executed.TrySetResult();
                    return ValueTask.CompletedTask;
                },
                static () => { },
                out _
            )
        );

        await queue.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        await executed.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(await queue.TryAbortRelocationAsync(seal));
    }

    [Fact]
    public async Task SerialExecutionQueue_Dispose_Joins_The_Drain_Epilogue_Before_Disposing_Its_Gate()
    {
        var exceptions = new ConcurrentQueue<Exception>();
        using var errorSink = new ZLinkRuntimeErrorSink();
        errorSink.UnhandledCallbackException += exceptions.Enqueue;
        try
        {
            for (var iteration = 0; iteration < 100; iteration++)
            {
                var runner = new ZLinkRuntimeTaskRunner(errorSink, CancellationToken.None);
                var queue = new ZLinkSerialExecutionQueue(
                    runner,
                    errorSink,
                    CancellationToken.None
                );
                Assert.True(queue.TryPost(static _ => ValueTask.CompletedTask, out var item));

                await item.Completion;
                await queue.DisposeAsync();
                await runner.StopAsync();
            }

            Assert.DoesNotContain(
                exceptions,
                static exception => exception is ObjectDisposedException
            );
        }
        finally
        {
            errorSink.UnhandledCallbackException -= exceptions.Enqueue;
        }
    }

    [Fact]
    public async Task StreamSessionSerialExecutor_Continues_After_Work_Exception()
    {
        var exceptions = new ConcurrentQueue<Exception>();
        using var errorSink = new ZLinkRuntimeErrorSink();
        errorSink.UnhandledCallbackException += exceptions.Enqueue;
        try
        {
            await using var executor = new ZLinkSessionSerialExecutor(new object(), errorSink);
            var completed = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );

            Assert.True(
                executor.ExecuteInfrastructure(() =>
                    throw new InvalidOperationException("stream failure")
                )
            );
            Assert.True(
                executor.ExecuteInfrastructure(() =>
                {
                    completed.SetResult();
                    return ValueTask.CompletedTask;
                })
            );

            await completed.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Contains(exceptions, static ex => ex.Message == "stream failure");
        }
        finally
        {
            errorSink.UnhandledCallbackException -= exceptions.Enqueue;
        }
    }

    [Fact]
    public async Task StreamSessionSerialExecutor_PreservesQueuedMessagesUntilTerminal()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = new ZLinkSessionSerialExecutor(new object(), errorSink);
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        Assert.Equal(
            ZLinkSerialPostAdmission.Accepted,
            executor.ExecuteApplication(async _ =>
            {
                started.TrySetResult();
                await release.Task.ConfigureAwait(false);
            })
        );
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(
            ZLinkSerialPostAdmission.Accepted,
            executor.ExecuteApplication(static _ => ValueTask.CompletedTask)
        );

        release.TrySetResult();
    }

    [Fact]
    public async Task SerialExecutionQueue_FinalTurn_BypassesCapacity_AndSealsAdmission()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var firstStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseFirst = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var finalRan = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        //  Release the pending first turn even when an assertion below fails —
        //  otherwise DisposeAsync waits on the drained signal forever and the
        //  failure surfaces as a hang instead of a red test.
        try
        {
            Assert.True(
                queue.TryPost(
                    async _ =>
                    {
                        firstStarted.TrySetResult();
                        await releaseFirst.Task.ConfigureAwait(false);
                    },
                    out _
                )
            );
            await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

            //  The serial queue no longer owns record-count capacity — that
            //  moved to the Application Job Queue admission (see the sibling
            //  SerialExecutionQueue_TryPost_AdmitsWhileApplicationWorkIsPending)
            //  — so an ordinary post while the first turn is pending is
            //  admitted, not refused.
            Assert.True(queue.TryPost(static _ => ValueTask.CompletedTask, out _));
            Assert.True(
                queue.TryPostFinal(
                    _ =>
                    {
                        finalRan.TrySetResult();
                        return ValueTask.CompletedTask;
                    },
                    out _
                )
            );
            Assert.False(queue.TryPost(static _ => ValueTask.CompletedTask, out _));
            Assert.False(queue.TryPostFinal(static _ => ValueTask.CompletedTask, out _));
        }
        finally
        {
            releaseFirst.TrySetResult();
        }

        await finalRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task SerialExecutionQueue_Cancellation_Joins_NonCooperative_Callback()
    {
        using var stop = new CancellationTokenSource();
        await using var queue = CreateQueue(stop.Token);
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        Assert.True(
            queue.TryPost(
                async _ =>
                {
                    started.TrySetResult();
                    await release.Task.ConfigureAwait(false);
                },
                out _
            )
        );
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));

        stop.Cancel();
        queue.Complete();
        var disposeTask = queue.DisposeAsync().AsTask();
        await Task.Delay(50);
        Assert.False(disposeTask.IsCompleted);

        release.TrySetResult();
        await disposeTask.WaitAsync(TimeSpan.FromSeconds(1));
    }

    [Fact]
    public async Task SerialExecutionQueue_Cancellation_Joins_Cooperative_Callback()
    {
        using var stop = new CancellationTokenSource();
        await using var queue = CreateQueue(stop.Token);
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var cancellationObserved = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        Assert.True(
            queue.TryPost(
                async cancellationToken =>
                {
                    started.TrySetResult();
                    try
                    {
                        await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken)
                            .ConfigureAwait(false);
                    }
                    catch (OperationCanceledException)
                        when (cancellationToken.IsCancellationRequested)
                    {
                        cancellationObserved.TrySetResult();
                        throw;
                    }
                },
                out _
            )
        );
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));

        stop.Cancel();
        queue.Complete();
        await queue.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(1));

        await cancellationObserved.Task.WaitAsync(TimeSpan.FromSeconds(1));
    }

    [Fact]
    public async Task SerialExecutionQueue_Suspended_Cancellation_Joins_Callback()
    {
        using var stop = new CancellationTokenSource();
        await using var queue = CreateQueue(stop.Token);
        var operationStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var operation = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var cancellationObserved = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseCallback = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        Assert.True(
            queue.TryPost(
                async cancellationToken =>
                {
                    var turn =
                        ZLinkSerialTurn.Current
                        ?? throw new InvalidOperationException("serial turn was not available");
                    try
                    {
                        await turn.YieldFrameworkCallAsync(
                                async _ =>
                                {
                                    operationStarted.TrySetResult();
                                    await operation.Task.ConfigureAwait(false);
                                },
                                cancellationToken
                            )
                            .ConfigureAwait(false);
                    }
                    catch when (cancellationToken.IsCancellationRequested)
                    {
                        cancellationObserved.TrySetResult();
                        await releaseCallback.Task.ConfigureAwait(false);
                    }
                },
                out _
            )
        );
        await operationStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        stop.Cancel();
        queue.Complete();
        var disposeTask = queue.DisposeAsync().AsTask();
        await cancellationObserved.Task.WaitAsync(TimeSpan.FromSeconds(1));
        await Task.Delay(50);
        Assert.False(disposeTask.IsCompleted);

        releaseCallback.TrySetResult();
        await disposeTask.WaitAsync(TimeSpan.FromSeconds(1));
        operation.TrySetResult();
    }

    [Fact]
    public async Task SpotSerialExecutor_Continues_After_Queued_Work_Exception()
    {
        var exceptions = new ConcurrentQueue<Exception>();
        using var errorSink = new ZLinkRuntimeErrorSink();
        errorSink.UnhandledCallbackException += exceptions.Enqueue;
        try
        {
            await using var executor = new ZLinkSpotSerialExecutor(
                null!,
                static () => false,
                CancellationToken.None,
                errorSink
            );
            var completed = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );

            executor.Queue(static (_, _) => throw new InvalidOperationException("spot failure"));
            executor.Queue(
                (_, _) =>
                {
                    completed.SetResult();
                    return ValueTask.CompletedTask;
                }
            );

            await completed.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Contains(exceptions, static ex => ex.Message == "spot failure");
        }
        finally
        {
            errorSink.UnhandledCallbackException -= exceptions.Enqueue;
        }
    }

    [Fact]
    public async Task SpotSerialExecutor_Queue_Reports_Skipped_Work_When_Stopped()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = new ZLinkSpotSerialExecutor(
            null!,
            static () => false,
            CancellationToken.None,
            errorSink
        );
        var skipped = 0;

        executor.RequestStop();

        Assert.False(executor.Queue(static (_, _) => ValueTask.CompletedTask, () => skipped++));
        Assert.False(executor.QueueNext(static (_, _) => ValueTask.CompletedTask, () => skipped++));
        Assert.Equal(2, skipped);
    }

    [Fact]
    public async Task SpotSerialExecutor_ExecuteAsync_Propagates_Work_Exception()
    {
        var exceptions = new ConcurrentQueue<Exception>();
        using var errorSink = new ZLinkRuntimeErrorSink();
        errorSink.UnhandledCallbackException += exceptions.Enqueue;
        try
        {
            await using (
                var executor = new ZLinkSpotSerialExecutor(
                    null!,
                    static () => false,
                    CancellationToken.None,
                    errorSink
                )
            )
            {
                var thrown = await Assert.ThrowsAsync<InvalidOperationException>(() =>
                    executor
                        .ExecuteAsync(
                            static (_, _) =>
                                throw new InvalidOperationException("spot execute failure"),
                            CancellationToken.None
                        )
                        .AsTask()
                );

                Assert.Equal("spot execute failure", thrown.Message);
            }
            Assert.Contains(exceptions, static ex => ex.Message == "spot execute failure");
        }
        finally
        {
            errorSink.UnhandledCallbackException -= exceptions.Enqueue;
        }
    }

    [Fact]
    public async Task SpotSerialExecutor_LifecycleCallback_Uses_ApplicationLane_For_ApplicationCode()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = new ZLinkSpotSerialExecutor(
            null!,
            static () => false,
            CancellationToken.None,
            errorSink
        );
        var applicationRan = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var beforeApplicationCallback = executor.LastApplicationWorkCompletedAt;
        await Task.Delay(10);

        var lifecycle = executor
            .ExecuteLifecycleAsync(
                async (_, cancellationToken) =>
                {
                    await executor
                        .ExecuteApplicationCallbackAsync(
                            static (_, completed, _) =>
                            {
                                Assert.True(
                                    ZLinkApplicationExecutionContext.Current
                                        is { YieldAllowed: true }
                                );
                                Assert.NotNull(ZLinkSerialTurn.Current);
                                completed.TrySetResult();
                                return ValueTask.CompletedTask;
                            },
                            applicationRan,
                            cancellationToken
                        )
                        .ConfigureAwait(false);
                },
                CancellationToken.None
            )
            .AsTask();

        await applicationRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await lifecycle.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(executor.LastApplicationWorkCompletedAt > beforeApplicationCallback);
    }

    [Fact]
    public async Task SerialExecutionQueue_RunAsync_Propagates_Work_Exception()
    {
        var exceptions = new ConcurrentQueue<Exception>();
        using var errorSink = new ZLinkRuntimeErrorSink();
        errorSink.UnhandledCallbackException += exceptions.Enqueue;
        try
        {
            await using var queue = CreateQueue(CancellationToken.None, errorSink: errorSink);

            var thrown = await Assert.ThrowsAsync<InvalidOperationException>(() =>
                queue
                    .RunAsync(
                        static _ => throw new InvalidOperationException("queue failure"),
                        CancellationToken.None
                    )
                    .AsTask()
            );

            Assert.Equal("queue failure", thrown.Message);
            Assert.Contains(exceptions, static ex => ex.Message == "queue failure");
        }
        finally
        {
            errorSink.UnhandledCallbackException -= exceptions.Enqueue;
        }
    }

    [Fact]
    public async Task SerialExecutionQueue_Wait_Cancellation_Does_Not_Remove_Queued_Work()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var firstStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseFirst = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var secondRan = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        var first = queue
            .RunAsync(
                async _ =>
                {
                    firstStarted.SetResult();
                    await releaseFirst.Task.ConfigureAwait(false);
                },
                CancellationToken.None
            )
            .AsTask();
        await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        using var waitCancellation = new CancellationTokenSource();
        var second = queue
            .RunAsync(
                _ =>
                {
                    secondRan.SetResult();
                    return ValueTask.CompletedTask;
                },
                waitCancellation.Token
            )
            .AsTask();
        waitCancellation.Cancel();

        try
        {
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                second.WaitAsync(TimeSpan.FromSeconds(5))
            );
            Assert.False(secondRan.Task.IsCompleted);
        }
        finally
        {
            releaseFirst.SetResult();
        }

        await first.WaitAsync(TimeSpan.FromSeconds(5));
        await secondRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task SerialExecutionQueue_ApplicationWorkQueuesUntilTerminal()
    {
        var policy = new ZLinkExecutionLanePolicy(1, TimeSpan.FromSeconds(1));
        await using var queue = CreateQueue(CancellationToken.None, policy: policy);
        await using var otherOwner = CreateQueue(CancellationToken.None, policy: policy);
        var releaseFirst = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        Assert.Equal(
            ZLinkSerialPostAdmission.Accepted,
            queue.TryPostApplicationWithAdmission(
                async _ => await releaseFirst.Task.ConfigureAwait(false),
                payloadBytes: 128,
                metadataBytes: 32,
                transferred: false,
                out var first
            )
        );
        Assert.Equal(1, queue.ApplicationPendingCount);
        Assert.Equal(
            ZLinkSerialPostAdmission.Accepted,
            queue.TryPostApplicationWithAdmission(
                static _ => ValueTask.CompletedTask,
                payloadBytes: 0,
                metadataBytes: 0,
                transferred: false,
                out var queued
            )
        );
        Assert.Equal(2, queue.ApplicationPendingCount);
        Assert.True(otherOwner.TryPost(static _ => ValueTask.CompletedTask, out var other));
        await other.Completion.WaitAsync(TimeSpan.FromSeconds(5));

        releaseFirst.SetResult();
        await first.Completion.WaitAsync(TimeSpan.FromSeconds(5));
        await queued.Completion.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(0, queue.ApplicationPendingCount);
        Assert.True(queue.TryPost(static _ => ValueTask.CompletedTask, out var following));
        await following.Completion.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task SerialExecutionQueue_ReportsClosedOnlyAfterCompletion()
    {
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(new ZLinkRuntimeErrorSink(), CancellationToken.None),
            new ZLinkRuntimeErrorSink(),
            CancellationToken.None
        );
        var releaseFirst = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        Assert.True(queue.TryPost(async _ => await releaseFirst.Task.ConfigureAwait(false), out _));
        Assert.Equal(
            ZLinkSerialPostAdmission.Accepted,
            queue.TryPostApplicationWithAdmission(static _ => ValueTask.CompletedTask, out _)
        );
        Assert.Equal(
            ZLinkSerialPostAdmission.Accepted,
            queue.TryPostNextWithAdmission(static _ => ValueTask.CompletedTask, out _)
        );

        releaseFirst.TrySetResult();
        queue.Complete();
        Assert.Equal(
            ZLinkSerialPostAdmission.Closed,
            queue.TryPostNextWithAdmission(static _ => ValueTask.CompletedTask, out _)
        );
    }

    [Fact]
    public async Task SerialExecutionQueue_Closes_Only_New_Application_Admission()
    {
        await using var queue = CreateQueue(CancellationToken.None);

        queue.CloseApplicationAdmission();

        Assert.Equal(
            ZLinkSerialPostAdmission.Closed,
            queue.TryPostApplicationWithAdmission(static _ => ValueTask.CompletedTask, out _)
        );
        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Closed,
            queue.TryPostAccepted(
                ReadOnlyMemory<byte>.Empty,
                static _ => ValueTask.CompletedTask,
                static () => { },
                out _
            )
        );
        Assert.Equal(
            ZLinkSerialPostAdmission.Accepted,
            queue.TryPostNextWithAdmission(static _ => ValueTask.CompletedTask, out _)
        );
    }

    [Fact]
    public async Task SerialExecutionQueue_TransferredWork_IsAccountedWithoutReadmission()
    {
        var policy = new ZLinkExecutionLanePolicy(1, TimeSpan.FromSeconds(1));
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(new ZLinkRuntimeErrorSink(), CancellationToken.None),
            new ZLinkRuntimeErrorSink(),
            CancellationToken.None,
            policy
        );

        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            queue.TryPostAccepted(
                new byte[48],
                async _ =>
                {
                    entered.TrySetResult();
                    await release.Task.ConfigureAwait(false);
                },
                static () => { },
                out var accepted
            )
        );
        await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            queue.TryPostAccepted(
                ReadOnlyMemory<byte>.Empty,
                static _ => ValueTask.CompletedTask,
                static () => { },
                out var transferred
            )
        );
        Assert.Equal(2, queue.ApplicationPendingCount);
        Assert.Equal(
            ZLinkSerialPostAdmission.Accepted,
            queue.TryPostApplicationWithAdmission(
                static _ => ValueTask.CompletedTask,
                out var queued
            )
        );

        Assert.Equal(
            ZLinkSerialPostAdmission.Accepted,
            queue.TryPostNextWithAdmission(static _ => ValueTask.CompletedTask, out var lifecycle)
        );
        Assert.Equal(1, queue.LifecyclePendingCount);

        release.TrySetResult();
        await Task.WhenAll(
                accepted.Completion,
                transferred.Completion,
                lifecycle.Completion,
                queued.Completion
            )
            .WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(0, queue.ApplicationPendingCount);
        Assert.Equal(0, queue.LifecyclePendingCount);
    }

    [Fact]
    public async Task SerialExecutionQueue_ByteMetadataDoesNotCreateAnAdmissionLimit()
    {
        var policy = new ZLinkExecutionLanePolicy(1, TimeSpan.FromSeconds(1));
        await using var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(new ZLinkRuntimeErrorSink(), CancellationToken.None),
            new ZLinkRuntimeErrorSink(),
            CancellationToken.None,
            policy
        );
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        Assert.Equal(
            ZLinkSerialPostAdmission.Accepted,
            queue.TryPostApplicationWithAdmission(
                async _ => await release.Task.ConfigureAwait(false),
                payloadBytes: 100,
                metadataBytes: 20,
                transferred: false,
                out var accepted
            )
        );
        Assert.Equal(
            ZLinkSerialPostAdmission.Accepted,
            queue.TryPostApplicationWithAdmission(
                static _ => ValueTask.CompletedTask,
                payloadBytes: 8,
                metadataBytes: 0,
                transferred: false,
                out var second
            )
        );
        Assert.Equal(2, queue.ApplicationPendingCount);
        Assert.Equal(
            ZLinkSerialPostAdmission.Accepted,
            queue.TryPostApplicationWithAdmission(
                static _ => ValueTask.CompletedTask,
                payloadBytes: long.MaxValue,
                metadataBytes: 1,
                transferred: false,
                out var overflow
            )
        );
        Assert.Equal(3, queue.ApplicationPendingCount);

        release.TrySetResult();
        await Task.WhenAll(accepted.Completion, second.Completion, overflow.Completion)
            .WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(
            ZLinkSerialPostAdmission.Accepted,
            queue.TryPostApplicationWithAdmission(
                static _ => ValueTask.CompletedTask,
                payloadBytes: 8,
                metadataBytes: 0,
                transferred: false,
                out var next
            )
        );
        await next.Completion.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task SerialExecutionQueue_ApplicationDrainedTracksEachBusyInterval()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var firstRelease = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        Assert.True(
            queue.TryPostApplication(
                async _ => await firstRelease.Task.ConfigureAwait(false),
                out var first
            )
        );
        var firstDrained = queue.ApplicationDrained;
        Assert.False(firstDrained.IsCompleted);

        firstRelease.TrySetResult();
        await first.Completion.WaitAsync(TimeSpan.FromSeconds(5));
        await firstDrained.WaitAsync(TimeSpan.FromSeconds(5));

        var secondRelease = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        Assert.True(
            queue.TryPostApplication(
                async _ => await secondRelease.Task.ConfigureAwait(false),
                out var second
            )
        );
        var secondDrained = queue.ApplicationDrained;
        Assert.NotSame(firstDrained, secondDrained);
        Assert.False(secondDrained.IsCompleted);

        secondRelease.TrySetResult();
        await second.Completion.WaitAsync(TimeSpan.FromSeconds(5));
        await secondDrained.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task SerialExecutionQueue_Selects_Lifecycle_Before_Ready_Application()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var firstStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseFirst = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var order = new ConcurrentQueue<string>();

        Assert.True(
            queue.TryPost(
                async _ =>
                {
                    firstStarted.TrySetResult();
                    await releaseFirst.Task.ConfigureAwait(false);
                    order.Enqueue("first");
                },
                out var first
            )
        );
        await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.True(
            queue.TryPostApplication(
                _ =>
                {
                    order.Enqueue("application");
                    return ValueTask.CompletedTask;
                },
                out var application
            )
        );
        Assert.True(
            queue.TryPostNext(
                _ =>
                {
                    order.Enqueue("lifecycle");
                    return ValueTask.CompletedTask;
                },
                out var lifecycle
            )
        );

        releaseFirst.TrySetResult();
        await Task.WhenAll(first.Completion, application.Completion, lifecycle.Completion)
            .WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(new[] { "first", "lifecycle", "application" }, order);
    }

    [Fact]
    public async Task SerialExecutionQueue_YieldDebt_Prevents_Lifecycle_Starvation_Of_Application()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var firstStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseFirst = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var order = new ConcurrentQueue<string>();

        Assert.True(
            queue.TryPostNext(
                async _ =>
                {
                    firstStarted.TrySetResult();
                    await releaseFirst.Task.ConfigureAwait(false);
                    order.Enqueue("lifecycle-0");
                },
                out var first
            )
        );
        await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var lifecycle = new List<ZLinkSerialWorkItem> { first };
        for (
            var index = 1;
            index < ZLinkExecutionLanePolicy.Default.LifecycleBurstLimit + 8;
            index++
        )
        {
            Assert.True(
                queue.TryPostNext(
                    _ =>
                    {
                        order.Enqueue($"lifecycle-{index}");
                        return ValueTask.CompletedTask;
                    },
                    out var item
                )
            );
            lifecycle.Add(item);
        }

        Assert.True(
            queue.TryPostApplication(
                _ =>
                {
                    order.Enqueue("application");
                    return ValueTask.CompletedTask;
                },
                out var application
            )
        );

        releaseFirst.TrySetResult();
        await Task.WhenAll(
                lifecycle.Select(static item => item.Completion).Append(application.Completion)
            )
            .WaitAsync(TimeSpan.FromSeconds(5));

        var applicationIndex = order.ToArray().ToList().IndexOf("application");
        Assert.InRange(applicationIndex, 0, ZLinkExecutionLanePolicy.Default.LifecycleBurstLimit);
    }

    [Fact]
    public async Task SerialExecutionQueue_PostAsync_Throws_AfterQueueCloses()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var releaseFirst = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        Assert.True(queue.TryPost(async _ => await releaseFirst.Task.ConfigureAwait(false), out _));

        queue.Complete();
        var exception = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            queue.PostAsync(_ => ValueTask.CompletedTask, CancellationToken.None).AsTask()
        );
        Assert.Equal(ZLinkFrameworkErrorKind.ShuttingDown, exception.Kind);

        releaseFirst.SetResult();
    }

    [Fact]
    public async Task SerialExecutionQueue_DefaultAwait_Holds_Gate_Until_Work_Completes()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var firstStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseFirst = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var secondRan = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        var first = queue
            .RunAsync(
                async _ =>
                {
                    firstStarted.SetResult();
                    await releaseFirst.Task.ConfigureAwait(false);
                },
                CancellationToken.None
            )
            .AsTask();
        await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var second = queue
            .RunAsync(
                _ =>
                {
                    secondRan.SetResult();
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None
            )
            .AsTask();

        await Task.Delay(100);
        Assert.False(secondRan.Task.IsCompleted);

        releaseFirst.SetResult();
        await Task.WhenAll(first, second).WaitAsync(TimeSpan.FromSeconds(5));
        await secondRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task SerialExecutionQueue_SuspendedLifecycle_RetainsFifoWhileApplicationRuns()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var applicationRan = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var laterLifecycleRan = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        var first = queue
            .RunLifecycleAsync(
                async ct =>
                {
                    var turn = ZLinkSerialTurn.Current!;
                    await turn.YieldFrameworkCallAsync(
                        async _ =>
                        {
                            entered.TrySetResult();
                            await release.Task.ConfigureAwait(false);
                        },
                        ct
                    );
                },
                CancellationToken.None
            )
            .AsTask();
        await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var second = queue
            .RunLifecycleAsync(
                _ =>
                {
                    laterLifecycleRan.TrySetResult();
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None
            )
            .AsTask();
        var application = queue
            .RunAsync(
                _ =>
                {
                    applicationRan.TrySetResult();
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None
            )
            .AsTask();
        try
        {
            await applicationRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(laterLifecycleRan.Task.IsCompleted);
        }
        finally
        {
            release.TrySetResult();
        }
        await application.WaitAsync(TimeSpan.FromSeconds(5));
        await first.WaitAsync(TimeSpan.FromSeconds(5));
        await second.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task SerialExecutionQueue_LifecycleContinuationRetainsOwnerAcrossTwoYields()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var firstWait = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var secondWait = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var firstStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var secondStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var laterRan = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        var first = queue
            .RunLifecycleAsync(
                async ct =>
                {
                    var turn = ZLinkSerialTurn.Current!;
                    await turn.YieldFrameworkCallAsync(
                        async _ =>
                        {
                            firstStarted.TrySetResult();
                            await firstWait.Task.ConfigureAwait(false);
                        },
                        ct
                    );
                    await turn.YieldFrameworkCallAsync(
                        async _ =>
                        {
                            secondStarted.TrySetResult();
                            await secondWait.Task.ConfigureAwait(false);
                        },
                        ct
                    );
                },
                CancellationToken.None
            )
            .AsTask();
        await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var later = queue
            .RunLifecycleAsync(
                _ =>
                {
                    laterRan.TrySetResult();
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None
            )
            .AsTask();
        try
        {
            firstWait.TrySetResult();
            await secondStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(laterRan.Task.IsCompleted);
        }
        finally
        {
            firstWait.TrySetResult();
            secondWait.TrySetResult();
        }
        await first.WaitAsync(TimeSpan.FromSeconds(5));
        await later.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task SerialExecutionQueue_SealDrainsAcceptedApplicationContinuationDuringLifecycleWait()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        using var sealCancellation = new CancellationTokenSource();
        var acceptedStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseAccepted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var lifecycleStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseLifecycle = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            queue.TryPostAccepted(
                ReadOnlyMemory<byte>.Empty,
                async ct =>
                {
                    await ZLinkSerialTurn.Current!.YieldFrameworkCallAsync(
                        async _ =>
                        {
                            acceptedStarted.TrySetResult();
                            await releaseAccepted.Task.ConfigureAwait(false);
                        },
                        ct
                    );
                },
                static () => { },
                out var accepted
            )
        );
        await acceptedStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var lifecycle = queue
            .RunLifecycleAsync(
                async ct =>
                {
                    await ZLinkSerialTurn.Current!.YieldFrameworkCallAsync(
                        async _ =>
                        {
                            lifecycleStarted.TrySetResult();
                            await releaseLifecycle.Task.ConfigureAwait(false);
                        },
                        ct
                    );
                },
                CancellationToken.None
            )
            .AsTask();
        await lifecycleStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var seal = queue.SealRelocationAsync(sealCancellation.Token).AsTask();
        try
        {
            releaseAccepted.TrySetResult();
            await accepted.Completion.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(seal.IsCompleted);
            releaseLifecycle.TrySetResult();
            await lifecycle.WaitAsync(TimeSpan.FromSeconds(5));
            var committed = await seal.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.True(await queue.TryAbortRelocationAsync(committed));
        }
        finally
        {
            sealCancellation.Cancel();
            releaseAccepted.TrySetResult();
            releaseLifecycle.TrySetResult();
        }
        await lifecycle.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task SerialExecutionQueue_AutomaticTurn_Allows_Later_Work_Then_Resumes_On_Line()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        var ioStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var completeIo = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var firstResumed = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseFirstResume = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var thirdRan = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var order = new ConcurrentQueue<string>();

        var first = queue
            .RunAsync(
                async ct =>
                {
                    order.Enqueue("first-start");
                    var turn =
                        ZLinkSerialTurn.Current
                        ?? throw new InvalidOperationException("serial turn was not available");
                    await turn.YieldFrameworkCallAsync(
                            async _ =>
                            {
                                ioStarted.SetResult();
                                await completeIo.Task.ConfigureAwait(false);
                            },
                            ct
                        )
                        .ConfigureAwait(false);
                    order.Enqueue("first-resumed");
                    firstResumed.SetResult();
                    await releaseFirstResume.Task.ConfigureAwait(false);
                },
                CancellationToken.None
            )
            .AsTask();

        await ioStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        await queue
            .RunAsync(
                _ =>
                {
                    order.Enqueue("second");
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None
            )
            .AsTask()
            .WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(new[] { "first-start", "second" }, order.ToArray());

        completeIo.SetResult();
        await firstResumed.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(1, queue.ApplicationPendingCount);

        var third = queue
            .RunAsync(
                _ =>
                {
                    order.Enqueue("third");
                    thirdRan.SetResult();
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None
            )
            .AsTask();

        await Task.Delay(100);
        Assert.False(thirdRan.Task.IsCompleted);

        releaseFirstResume.SetResult();
        await Task.WhenAll(first, third).WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(new[] { "first-start", "second", "first-resumed", "third" }, order.ToArray());
    }

    [Fact]
    public async Task SerialExecutionQueue_AutomaticTurn_Fault_Cleans_Pending_Turn()
    {
        var exceptions = new ConcurrentQueue<Exception>();
        using var errorSink = new ZLinkRuntimeErrorSink();
        errorSink.UnhandledCallbackException += exceptions.Enqueue;
        try
        {
            await using var queue = CreateQueue(CancellationToken.None, errorSink: errorSink);
            var ioStarted = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            var failIo = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            var secondRan = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            var thirdRan = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );

            var first = queue
                .RunAsync(
                    async ct =>
                    {
                        var turn =
                            ZLinkSerialTurn.Current
                            ?? throw new InvalidOperationException("serial turn was not available");
                        await turn.YieldFrameworkCallAsync(
                                async _ =>
                                {
                                    ioStarted.SetResult();
                                    await failIo.Task.ConfigureAwait(false);
                                },
                                ct
                            )
                            .ConfigureAwait(false);
                    },
                    CancellationToken.None
                )
                .AsTask();

            await ioStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

            await queue
                .RunAsync(
                    _ =>
                    {
                        secondRan.SetResult();
                        return ValueTask.CompletedTask;
                    },
                    CancellationToken.None
                )
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(5));

            failIo.SetException(new InvalidOperationException("yield I/O failed"));

            var thrown = await Assert.ThrowsAsync<InvalidOperationException>(() =>
                first.WaitAsync(TimeSpan.FromSeconds(5))
            );
            Assert.Equal("yield I/O failed", thrown.Message);

            await queue
                .RunAsync(
                    _ =>
                    {
                        thirdRan.SetResult();
                        return ValueTask.CompletedTask;
                    },
                    CancellationToken.None
                )
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(5));

            await secondRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await thirdRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Contains(exceptions, static ex => ex.Message == "yield I/O failed");
        }
        finally
        {
            errorSink.UnhandledCallbackException -= exceptions.Enqueue;
        }
    }

    [Fact]
    public async Task SerialExecutionQueue_AutomaticTurn_Cancellation_Cleans_Pending_Turn()
    {
        var exceptions = new ConcurrentQueue<Exception>();
        using var errorSink = new ZLinkRuntimeErrorSink();
        errorSink.UnhandledCallbackException += exceptions.Enqueue;
        try
        {
            await using var queue = CreateQueue(CancellationToken.None, errorSink: errorSink);
            var ioStarted = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            var cancelIo = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            var secondRan = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            var thirdRan = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );

            var first = queue
                .RunAsync(
                    async ct =>
                    {
                        var turn =
                            ZLinkSerialTurn.Current
                            ?? throw new InvalidOperationException("serial turn was not available");
                        await turn.YieldFrameworkCallAsync(
                                async _ =>
                                {
                                    ioStarted.SetResult();
                                    await cancelIo.Task.ConfigureAwait(false);
                                },
                                ct
                            )
                            .ConfigureAwait(false);
                    },
                    CancellationToken.None
                )
                .AsTask();

            await ioStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

            await queue
                .RunAsync(
                    _ =>
                    {
                        secondRan.SetResult();
                        return ValueTask.CompletedTask;
                    },
                    CancellationToken.None
                )
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(5));

            cancelIo.SetCanceled();

            await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                first.WaitAsync(TimeSpan.FromSeconds(5))
            );

            await queue
                .RunAsync(
                    _ =>
                    {
                        thirdRan.SetResult();
                        return ValueTask.CompletedTask;
                    },
                    CancellationToken.None
                )
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(5));

            await secondRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await thirdRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.DoesNotContain(exceptions, static ex => ex is OperationCanceledException);
        }
        finally
        {
            errorSink.UnhandledCallbackException -= exceptions.Enqueue;
        }
    }

    [Fact]
    public async Task SerialExecutionQueue_AutomaticTurn_Caller_Cancellation_Allows_Later_Work_Before_Operation_Completes()
    {
        await using var queue = CreateQueue(CancellationToken.None);
        using var cancelWait = new CancellationTokenSource();
        var ioStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var completeIo = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var secondRan = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var thirdRan = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var order = new ConcurrentQueue<string>();

        var first = queue
            .RunAsync(
                async ct =>
                {
                    var turn =
                        ZLinkSerialTurn.Current
                        ?? throw new InvalidOperationException("serial turn was not available");
                    try
                    {
                        await turn.YieldFrameworkCallAsync(
                                async _ =>
                                {
                                    ioStarted.SetResult();
                                    await completeIo.Task.ConfigureAwait(false);
                                },
                                cancelWait.Token
                            )
                            .ConfigureAwait(false);
                    }
                    catch (OperationCanceledException)
                    {
                        order.Enqueue("first-cancelled");
                    }
                },
                CancellationToken.None
            )
            .AsTask();

        await ioStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        await queue
            .RunAsync(
                _ =>
                {
                    order.Enqueue("second");
                    secondRan.SetResult();
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None
            )
            .AsTask()
            .WaitAsync(TimeSpan.FromSeconds(5));

        await cancelWait.CancelAsync();
        await first.WaitAsync(TimeSpan.FromSeconds(5));

        await queue
            .RunAsync(
                _ =>
                {
                    order.Enqueue("third");
                    thirdRan.SetResult();
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None
            )
            .AsTask()
            .WaitAsync(TimeSpan.FromSeconds(5));

        Assert.False(completeIo.Task.IsCompleted);
        completeIo.SetResult();
        await secondRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await thirdRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(new[] { "second", "first-cancelled", "third" }, order.ToArray());
    }

    [Fact]
    public async Task ActorDispatchCancellation_Does_Not_Stop_Current_Or_Later_Dispatch()
    {
        var state = new ZLinkActorRuntimeState("test-actor");
        var firstStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseFirst = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var secondRan = false;
        var thirdRan = false;

        var first = state
            .ExecuteDispatchAsync(
                async _ =>
                {
                    firstStarted.SetResult();
                    await releaseFirst.Task.ConfigureAwait(false);
                },
                CancellationToken.None
            )
            .AsTask();
        await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        using var secondCancellation = new CancellationTokenSource();
        var second = state
            .ExecuteDispatchAsync(
                _ =>
                {
                    secondRan = true;
                    return ValueTask.CompletedTask;
                },
                secondCancellation.Token
            )
            .AsTask();
        await secondCancellation.CancelAsync();

        await Assert.ThrowsAsync<OperationCanceledException>(() =>
            second.WaitAsync(TimeSpan.FromSeconds(5))
        );
        Assert.False(secondRan);

        releaseFirst.SetResult();
        await first.WaitAsync(TimeSpan.FromSeconds(5));

        await state
            .ExecuteDispatchAsync(
                _ =>
                {
                    thirdRan = true;
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None
            )
            .AsTask()
            .WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(thirdRan);
    }

    [Fact]
    public async Task ActorDispatchMailbox_Runs_Waiters_In_Fifo_Order()
    {
        var state = new ZLinkActorRuntimeState("test-actor");
        var firstStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseFirst = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var order = new List<string>();

        var first = state
            .ExecuteDispatchAsync(
                async _ =>
                {
                    order.Add("first");
                    firstStarted.SetResult();
                    await releaseFirst.Task.ConfigureAwait(false);
                },
                CancellationToken.None
            )
            .AsTask();
        await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var second = state
            .ExecuteDispatchAsync(
                _ =>
                {
                    order.Add("second");
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None
            )
            .AsTask();
        var third = state
            .ExecuteDispatchAsync(
                _ =>
                {
                    order.Add("third");
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None
            )
            .AsTask();

        releaseFirst.SetResult();

        await Task.WhenAll(first, second, third).WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(["first", "second", "third"], order);
    }

    private static ZlinkStreamHeader CreateHeader(string name)
    {
        return new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.None,
            null,
            name,
            ZlinkStreamMetadata.Empty
        );
    }

    private static async Task WaitUntilAsync(Func<bool> condition, TimeSpan timeout)
    {
        using var cancellation = new CancellationTokenSource(timeout);
        while (!condition())
            await Task.Delay(10, cancellation.Token);
    }

    private static ZLinkSerialExecutionQueue CreateQueue(
        CancellationToken executionToken,
        ZLinkRuntimeErrorSink? errorSink = null,
        ZLinkExecutionLanePolicy? policy = null
    )
    {
        errorSink ??= new ZLinkRuntimeErrorSink();
        return new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errorSink, executionToken),
            errorSink,
            executionToken,
            policy ?? ZLinkExecutionLanePolicy.Default
        );
    }
}
