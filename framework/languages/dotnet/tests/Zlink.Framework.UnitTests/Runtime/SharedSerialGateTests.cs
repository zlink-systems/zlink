using System.Collections.Concurrent;

namespace Zlink.Framework.UnitTests;

public sealed class SharedSerialGateTests
{
    [Fact]
    public async Task IdleActorGateUsesOneAcquireRmwAndNoConsumerAdmissionLock()
    {
        using var errors = new ZLinkRuntimeErrorSink();
        var operations = new ConcurrentQueue<ZLinkSerialGateOperation>();
        await using var executor = new ZLinkSpotSerialExecutor(
            null!,
            static () => false,
            CancellationToken.None,
            errors,
            executionGateObserved: operations.Enqueue
        );
        var entered = Signal();
        var release = Signal();
        var execution = executor
            .ExecuteActorAsync(
                "actor",
                async (_, _, _) =>
                {
                    entered.TrySetResult();
                    await release.Task.ConfigureAwait(false);
                },
                0,
                CancellationToken.None
            )
            .AsTask();
        try
        {
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(new[] { ZLinkSerialGateOperation.Acquire }, operations.ToArray());
        }
        finally
        {
            release.TrySetResult();
            await execution.WaitAsync(TimeSpan.FromSeconds(5));
        }
    }

    [Fact]
    public async Task ConcurrentPublicationsHaveOneSequenceAndSealBoundary()
    {
        using var errors = new ZLinkRuntimeErrorSink();
        var runner = new ZLinkRuntimeTaskRunner(errors, CancellationToken.None);
        await using var queue = new ZLinkSerialExecutionQueue(
            runner,
            errors,
            CancellationToken.None,
            ZLinkExecutionLanePolicy.Default,
            sharedGate: true
        );
        var entered = Signal();
        var release = Signal();
        var blocker = queue
            .RunAsync(
                async _ =>
                {
                    entered.TrySetResult();
                    await release.Task.ConfigureAwait(false);
                },
                CancellationToken.None
            )
            .AsTask();
        await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var published = new ConcurrentBag<ZLinkSerialWorkItem>();
        var releases = 0;
        var executions = 0;
        try
        {
            await Task.WhenAll(
                    Enumerable
                        .Range(0, 4)
                        .Select(producer =>
                            Task.Run(() =>
                            {
                                for (var index = 0; index < 64; index++)
                                {
                                    Assert.Equal(
                                        ZLinkAcceptedWorkAdmission.Accepted,
                                        queue.TryPostAccepted(
                                            new byte[] { (byte)producer, (byte)index },
                                            _ =>
                                            {
                                                Interlocked.Increment(ref executions);
                                                return ValueTask.CompletedTask;
                                            },
                                            () => Interlocked.Increment(ref releases),
                                            out var item
                                        )
                                    );
                                    published.Add(item);
                                }
                            })
                        )
                )
                .WaitAsync(TimeSpan.FromSeconds(5));
            var sealing = queue.SealRelocationAsync(CancellationToken.None).AsTask();
            Assert.False(sealing.IsCompleted);
            release.TrySetResult();
            var seal = await sealing.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(
                Enumerable.Range(1, 256).Select(value => (ulong)value),
                seal.Captured.Select(record => record.AcceptedSequence)
            );
            Assert.Equal(
                published
                    .OrderBy(item => item.AcceptedSequence)
                    .Select(item => item.AcceptedPayload.ToArray()),
                seal.Captured.Select(record => record.Payload.ToArray())
            );
            Assert.Equal(0, Volatile.Read(ref executions));
            Assert.Equal(
                ZLinkAcceptedWorkAdmission.Accepted,
                queue.TryPostAccepted(
                    new byte[] { 255 },
                    static _ => ValueTask.CompletedTask,
                    () => Interlocked.Increment(ref releases),
                    out var heldItem
                )
            );
            Assert.True(queue.TryFreezeRelocationIngress(seal, out var frozen));
            Assert.Equal((ulong)257, Assert.Single(frozen).AcceptedSequence);
            Assert.Equal(
                ZLinkAcceptedWorkAdmission.RelocationMoving,
                queue.TryPostAccepted(
                    new byte[] { 0 },
                    static _ => ValueTask.CompletedTask,
                    static () => { },
                    out _
                )
            );
            Assert.True(queue.TryCommitRelocation(seal, out var held));
            Assert.Equal(frozen, held);
            await Task.WhenAll(
                    published.Select(item => item.Completion).Append(heldItem.Completion)
                )
                .WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(257, Volatile.Read(ref releases));
            Assert.Equal(0, queue.ApplicationPendingCount);
        }
        finally
        {
            release.TrySetResult();
            await blocker.WaitAsync(TimeSpan.FromSeconds(5));
        }
    }

    [Fact]
    public async Task PublicationAndIdleTransitionsDoNotStrandRecords()
    {
        using var errors = new ZLinkRuntimeErrorSink();
        await using var executor = new ZLinkSpotSerialExecutor(
            null!,
            static () => false,
            CancellationToken.None,
            errors
        );
        var executions = 0;
        for (var round = 0; round < 64; round++)
        {
            var calls = Enumerable
                .Range(0, 4)
                .Select(actor =>
                    Task.Run(async () =>
                        await executor.ExecuteActorAsync(
                            actor.ToString(),
                            (_, _, _) =>
                            {
                                Interlocked.Increment(ref executions);
                                return ValueTask.CompletedTask;
                            },
                            0,
                            CancellationToken.None
                        )
                    )
                )
                .ToArray();
            await Task.WhenAll(calls).WaitAsync(TimeSpan.FromSeconds(5));
        }
        Assert.Equal(256, Volatile.Read(ref executions));
    }

    [Fact]
    public async Task AdmissionStateReadDoesNotWaitForAnOrdinaryHandler()
    {
        using var errors = new ZLinkRuntimeErrorSink();
        var runner = new ZLinkRuntimeTaskRunner(errors, CancellationToken.None);
        await using var queue = new ZLinkSerialExecutionQueue(
            runner,
            errors,
            CancellationToken.None,
            ZLinkExecutionLanePolicy.Default,
            sharedGate: true
        );
        var entered = Signal();
        var release = Signal();
        var activeEntered = Signal();
        var releaseActive = Signal();
        var blocker = queue
            .RunAsync(
                async _ =>
                {
                    entered.TrySetResult();
                    await release.Task;
                },
                CancellationToken.None
            )
            .AsTask();
        await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        try
        {
            Assert.Equal(
                ZLinkAcceptedWorkAdmission.Accepted,
                queue.TryPostAccepted(
                    1,
                    static () => new byte[] { 1 },
                    async _ =>
                    {
                        activeEntered.TrySetResult();
                        await releaseActive.Task;
                    },
                    static () => { },
                    false,
                    out var active,
                    acceptedState: "active"
                )
            );
            Assert.Equal(
                ZLinkAcceptedWorkAdmission.Accepted,
                queue.TryPostAccepted(
                    1,
                    static () => new byte[] { 2 },
                    static _ => ValueTask.CompletedTask,
                    static () => { },
                    false,
                    out var pending,
                    acceptedState: "pending"
                )
            );
            Assert.True(
                await Task.Run(() =>
                        queue.HasPendingAcceptedState(state => Equals(state, "pending"))
                    )
                    .WaitAsync(TimeSpan.FromSeconds(5))
            );
            release.TrySetResult();
            await activeEntered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(
                await Task.Run(() =>
                        queue.HasPendingAcceptedState(state => Equals(state, "active"))
                    )
                    .WaitAsync(TimeSpan.FromSeconds(5))
            );
            Assert.True(
                await Task.Run(() =>
                        queue.HasPendingAcceptedState(state => Equals(state, "pending"))
                    )
                    .WaitAsync(TimeSpan.FromSeconds(5))
            );
            releaseActive.TrySetResult();
            await Task.WhenAll(active.Completion, pending.Completion)
                .WaitAsync(TimeSpan.FromSeconds(5));
        }
        finally
        {
            release.TrySetResult();
            releaseActive.TrySetResult();
            await blocker.WaitAsync(TimeSpan.FromSeconds(5));
        }
    }

    [Fact]
    public async Task PublicationAtTheIdleBoundaryIsConsumedOnce()
    {
        using var errors = new ZLinkRuntimeErrorSink();
        var runner = new ZLinkRuntimeTaskRunner(errors, CancellationToken.None);
        await using var queue = new ZLinkSerialExecutionQueue(
            runner,
            errors,
            CancellationToken.None,
            ZLinkExecutionLanePolicy.Default,
            sharedGate: true
        );
        var releasing = Signal();
        using var published = new ManualResetEventSlim();
        var releases = 0;
        var executions = 0;
        queue.GateOperation = operation =>
        {
            if (
                operation == ZLinkSerialGateOperation.Release
                && Interlocked.Increment(ref releases) == 1
            )
            {
                releasing.TrySetResult();
                Assert.True(published.Wait(TimeSpan.FromSeconds(5)));
            }
        };
        var first = queue
            .RunAsync(static _ => ValueTask.CompletedTask, CancellationToken.None)
            .AsTask();
        try
        {
            await releasing.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var second = Task.Run(() =>
            {
                Assert.True(
                    queue.TryPostApplication(
                        _ =>
                        {
                            Interlocked.Increment(ref executions);
                            return ValueTask.CompletedTask;
                        },
                        out var item
                    )
                );
                published.Set();
                return item.Completion;
            });
            await second.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(1, Volatile.Read(ref executions));
        }
        finally
        {
            published.Set();
            await first.WaitAsync(TimeSpan.FromSeconds(5));
        }
    }

    [Fact]
    public async Task IngressMailboxTransfersNextClaimAfterCurrentTerminal()
    {
        using var errors = new ZLinkRuntimeErrorSink();
        var runner = new ZLinkRuntimeTaskRunner(errors, CancellationToken.None);
        await using var source = new ZLinkSerialExecutionQueue(
            runner,
            errors,
            CancellationToken.None,
            ZLinkExecutionLanePolicy.Default,
            sharedGate: true
        );
        await using var target = new ZLinkSerialExecutionQueue(
            runner,
            errors,
            CancellationToken.None,
            ZLinkExecutionLanePolicy.Default,
            sharedGate: true
        );
        ZLinkSerialExecutionQueue? owner = source;
        var mailbox = new ZLinkActorSerialExecutor();
        mailbox.BindExecutionOwner(() => Volatile.Read(ref owner));
        var current = await mailbox.EnterAsync(CancellationToken.None);
        var next = mailbox.EnterAsync(CancellationToken.None).AsTask();
        var targetEntered = Signal();
        var targetRelease = Signal();
        var blocker = target
            .RunAsync(
                async _ =>
                {
                    targetEntered.TrySetResult();
                    await targetRelease.Task.ConfigureAwait(false);
                },
                CancellationToken.None
            )
            .AsTask();
        await targetEntered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Volatile.Write(ref owner, target);
        current.Dispose();
        try
        {
            await Assert.ThrowsAsync<TimeoutException>(() =>
                next.WaitAsync(TimeSpan.FromMilliseconds(100))
            );
        }
        finally
        {
            targetRelease.TrySetResult();
            await blocker.WaitAsync(TimeSpan.FromSeconds(5));
        }
        using (await next.WaitAsync(TimeSpan.FromSeconds(5))) { }
        // Returning to PerActor relinquishes the shared consumer after terminal.
        Volatile.Write(ref owner, null);
        using (
            await mailbox
                .EnterAsync(CancellationToken.None)
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(5))
        ) { }
    }

    [Fact]
    public async Task LifecycleResumePublicationIsRejectedBySharedConsumer()
    {
        using var errors = new ZLinkRuntimeErrorSink();
        var runner = new ZLinkRuntimeTaskRunner(errors, CancellationToken.None);
        await using var queue = new ZLinkSerialExecutionQueue(
            runner,
            errors,
            CancellationToken.None,
            ZLinkExecutionLanePolicy.Default,
            sharedGate: true
        );
        var external = Signal();
        var suspended = Signal();
        var busy = Signal();
        var release = Signal();
        var published = Signal();
        var notifications = 0;
        var lifecycle = queue
            .RunLifecycleAsync(
                async _ =>
                {
                    var turn = ZLinkSerialTurn.Current!;
                    var first = turn.YieldFrameworkCallAsync(
                            _ => new ValueTask(external.Task),
                            CancellationToken.None
                        )
                        .AsTask();
                    var duplicate = turn.YieldFrameworkCallAsync(
                            _ => new ValueTask(external.Task),
                            CancellationToken.None
                        )
                        .AsTask();
                    suspended.TrySetResult();
                    var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                        Task.WhenAll(first, duplicate)
                    );
                    Assert.Equal(ZLinkFrameworkErrorKind.ShuttingDown, failure.Kind);
                },
                CancellationToken.None
            )
            .AsTask();
        await suspended.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var blocker = queue
            .RunAsync(
                async _ =>
                {
                    busy.TrySetResult();
                    await release.Task.ConfigureAwait(false);
                },
                CancellationToken.None
            )
            .AsTask();
        await busy.Task.WaitAsync(TimeSpan.FromSeconds(5));
        queue.GateOperation = operation =>
        {
            if (
                operation == ZLinkSerialGateOperation.Notify
                && Interlocked.Increment(ref notifications) == 2
            )
                published.TrySetResult();
        };
        try
        {
            external.TrySetResult();
            await published.Task.WaitAsync(TimeSpan.FromSeconds(5));
        }
        finally
        {
            release.TrySetResult();
        }
        await Task.WhenAll(blocker, lifecycle).WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task IngressPromotionDoesNotClearNewSharedConsumersYieldClaim()
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
            CancellationToken.None
        );
        mailbox.BindExecutionOwner(() => owner);
        using var entered = new ManualResetEventSlim();
        var external = Signal();
        var transferred = Signal();
        var nextEntered = Signal();
        mailbox.GateOperation = operation =>
        {
            if (operation != ZLinkSerialGateOperation.ConsumerTransfer)
                return;
            // Publish while the previous PerActor consumer is finishing its transfer.
            mailbox.NotifyReadiness();
            Assert.True(entered.Wait(TimeSpan.FromSeconds(5)));
            transferred.TrySetResult();
        };
        var first = mailbox
            .RunAsync(
                async _ =>
                {
                    entered.Set();
                    await ZLinkSerialTurn.Current!.YieldFrameworkCallAsync(
                        _ => new ValueTask(external.Task),
                        CancellationToken.None
                    );
                },
                CancellationToken.None
            )
            .AsTask();
        await transferred.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var next = mailbox
            .RunAsync(
                _ =>
                {
                    nextEntered.TrySetResult();
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None
            )
            .AsTask();
        try
        {
            await Assert.ThrowsAsync<TimeoutException>(() =>
                nextEntered.Task.WaitAsync(TimeSpan.FromMilliseconds(100))
            );
        }
        finally
        {
            external.TrySetResult();
            await Task.WhenAll(first, next).WaitAsync(TimeSpan.FromSeconds(5));
        }
    }

    private static TaskCompletionSource Signal() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);
}
