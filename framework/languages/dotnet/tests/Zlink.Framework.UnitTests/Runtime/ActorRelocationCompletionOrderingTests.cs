using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ActorRelocationCompletionOrderingTests
{
    private static TaskCompletionSource Signal() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Durable_failure_reaches_the_runtime_reporter_once(bool preparationFails)
    {
        var mailbox = new ZLinkActorSerialExecutor();
        var failure = new InvalidOperationException("durable record failed");
        var reported = new TaskCompletionSource<Exception>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var reports = 0;
        var terminal = mailbox.AdmitDurablePrefix(
            new[]
            {
                new ZLinkActorDurableRecord(
                    _ => throw failure,
                    _ =>
                        preparationFails
                            ? ValueTask.FromException(failure)
                            : ValueTask.CompletedTask,
                    ReportFailure: error =>
                    {
                        reports++;
                        reported.TrySetResult(error);
                    }
                ),
            }
        );
        Assert.Same(
            failure,
            await Assert.ThrowsAsync<InvalidOperationException>(() => Task.WhenAll(terminal))
        );
        Assert.Same(failure, await reported.Task);
        Assert.Equal(1, reports);
    }

    [Fact]
    public async Task Completed_durable_turn_cannot_bypass_a_later_actor_turn()
    {
        var mailbox = new ZLinkActorSerialExecutor();
        ExecutionContext? captured = null;
        var prefix = mailbox.AdmitDurablePrefix(
            new[]
            {
                new ZLinkActorDurableRecord(_ =>
                {
                    captured = ExecutionContext.Capture();
                    return ValueTask.CompletedTask;
                }),
            }
        );
        await Task.WhenAll(prefix);
        var owner = await mailbox.EnterAsync(default);
        Task<ZLinkActorSerialExecutor.Turn>? late = null;
        ExecutionContext.Run(captured!, _ => late = mailbox.EnterAsync(default).AsTask(), null);
        var bypassed = late!.IsCompleted;
        owner.Dispose();
        using (await late) { }
        Assert.False(bypassed);
    }

    [Fact]
    public async Task Ingress_after_the_last_durable_claim_keeps_its_ordinary_initial_permit()
    {
        using var host = new ZLinkApplicationJobQueue(
            new(ZLinkApplicationJobQueueProfile.Balanced, 1, 1, 1)
        );
        var mailbox = new ZLinkActorSerialExecutor();
        var entered = Signal();
        var finish = Signal();
        var order = new List<string>();
        ZLinkApplicationJobQueueLease? prepared = null;
        var terminal = mailbox.AdmitDurablePrefix(
            new[]
            {
                new ZLinkActorDurableRecord(
                    async _ =>
                    {
                        using var invocation = ZLinkApplicationJobQueueInvocation.Enter(prepared!);
                        await ZLinkApplicationJobQueueInvocation.ReleaseForHandlerStartAsync();
                        order.Add("last-prefix");
                        entered.SetResult();
                        await finish.Task;
                    },
                    async ct =>
                    {
                        prepared = await host.AcquireAsync(ct, ZLinkApplicationJobOrigin.Remote);
                    },
                    () => Interlocked.Exchange(ref prepared, null)?.Dispose()
                ),
            }
        );
        await entered.Task;
        using var initial = await host.AcquireAsync(default, ZLinkApplicationJobOrigin.Remote);
        async Task Ingress()
        {
            using var invocation = ZLinkApplicationJobQueueInvocation.Enter(initial);
            using var turn = await mailbox.EnterAsync(default);
            await ZLinkApplicationJobQueueInvocation.ReleaseForHandlerStartAsync();
            order.Add("ordinary");
        }
        var q = Ingress();
        var permits = host.GetStatus().PermitsInUse;
        finish.SetResult();
        await Task.WhenAll(terminal.Append(q));
        Assert.Equal(1UL, permits);
        Assert.Equal(new[] { "last-prefix", "ordinary" }, order);
        Assert.Equal(0UL, host.GetStatus().PermitsInUse);
    }

    [Fact]
    public async Task Join_completion_keeps_its_lifecycle_position_ahead_of_the_durable_prefix()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        state.BindNativeActorRef(new ZLinkBackendActorRef(RoutingId.From("target"), "actor-1", 7));
        state.BindActorInstance(new TestActor("actor-1"));
        var entered = Signal();
        var finish = Signal();
        var runnable = Signal();
        var order = new List<string>();
        var parent = state
            .ExecuteLifecycleAsync(
                async _ =>
                {
                    entered.SetResult();
                    await finish.Task;
                },
                default
            )
            .AsTask();
        await entered.Task;
        var prefix = state.AdmitDurablePrefix(
            new[]
            {
                new ZLinkActorDurableRecord(
                    _ =>
                    {
                        order.Add("handler");
                        return ValueTask.CompletedTask;
                    },
                    ct => new ValueTask(runnable.Task.WaitAsync(ct))
                ),
            }
        );
        var completion = state
            .ExecuteRelocationCompletionAsync(
                7,
                _ =>
                {
                    order.Add("completion");
                    return ValueTask.CompletedTask;
                },
                default
            )
            .AsTask();
        var lifecyclePositions = state.PendingLifecycleCount;
        runnable.SetResult();
        finish.SetResult();
        await Task.WhenAll(prefix.Append(parent).Append(completion));
        Assert.Equal(2, lifecyclePositions);
        Assert.Equal(new[] { "completion", "handler" }, order);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Withdrawn_durable_readiness_releases_waiter_and_prepared_permit_once(
        bool preparedPhase
    )
    {
        using var host = new ZLinkApplicationJobQueue(
            new(ZLinkApplicationJobQueueProfile.Balanced, 1, 1, 1)
        );
        using var blocker = preparedPhase
            ? null
            : await host.AcquireAsync(default, ZLinkApplicationJobOrigin.Local);
        var mailbox = new ZLinkActorSerialExecutor();
        var atPhase = Signal();
        var preparationFinished = Signal();
        var releaseClaim = Signal();
        ZLinkApplicationJobQueueLease? permit = null;
        Func<ValueTask<bool>>? withdraw = null;
        var releases = 0;
        var executed = 0;
        var records = mailbox.AdmitDurablePrefix(
            new[]
            {
                new ZLinkActorDurableRecord(
                    _ =>
                    {
                        executed++;
                        return ValueTask.CompletedTask;
                    },
                    async ct =>
                    {
                        try
                        {
                            var acquisition = host.AcquireAsync(
                                ct,
                                ZLinkApplicationJobOrigin.Remote
                            );
                            if (!preparedPhase)
                                atPhase.SetResult();
                            permit = await acquisition;
                            if (preparedPhase)
                                atPhase.SetResult();
                            await releaseClaim.Task.WaitAsync(ct);
                        }
                        finally
                        {
                            preparationFinished.SetResult();
                        }
                    },
                    () =>
                    {
                        if (Interlocked.Exchange(ref permit, null) is { } owned)
                        {
                            owned.Dispose();
                            releases++;
                        }
                    },
                    OnWithdrawable: take => withdraw = take
                ),
            }
        );
        await atPhase.Task;
        Assert.Equal(preparedPhase ? 0UL : 1UL, host.GetStatus().CapacityWaiters);
        Assert.True(await withdraw!());
        Assert.False(await withdraw());
        await Task.WhenAll(records);
        await preparationFinished.Task;
        Assert.Equal(0, executed);
        Assert.Equal(preparedPhase ? 1 : 0, releases);
        Assert.Equal(0UL, host.GetStatus().CapacityWaiters);
        blocker?.Dispose();
        Assert.Equal(0UL, host.GetStatus().PermitsInUse);
    }

    [Fact]
    public async Task Previous_move_finishes_before_first_handler_registers_the_next_join()
    {
        var events = new List<string>();
        var routeSubmitted = Signal();
        var releaseRoute = Signal();
        var activation = ZLinkActorTargetActivation
            .CompleteAsync(
                () =>
                {
                    events.Add("publication-clear");
                    return ValueTask.CompletedTask;
                },
                () =>
                {
                    events.Add("prefix-owned");
                    return ValueTask.CompletedTask;
                },
                () =>
                {
                    events.Add("join-completed");
                    return ValueTask.CompletedTask;
                },
                async () =>
                {
                    events.Add("command44");
                    routeSubmitted.SetResult();
                    await releaseRoute.Task;
                },
                () =>
                {
                    events.Add("handler");
                    events.Add("next-join");
                    return ValueTask.CompletedTask;
                }
            )
            .AsTask();
        await routeSubmitted.Task;
        var beforeRouteCompletion = events.ToArray();
        releaseRoute.SetResult();
        await activation;
        Assert.Equal(
            new[] { "publication-clear", "prefix-owned", "join-completed", "command44" },
            beforeRouteCompletion
        );
        Assert.Equal(
            new[]
            {
                "publication-clear",
                "prefix-owned",
                "join-completed",
                "command44",
                "handler",
                "next-join",
            },
            events
        );
    }

    [Fact]
    public async Task Previous_normalization_finishes_before_completion_creates_the_next_root()
    {
        var events = new List<string>();
        var root = "first-root";
        var completed = Signal();
        var continueCompletion = Signal();
        var activation = ZLinkActorTargetActivation
            .CompleteAsync(
                () =>
                {
                    events.Add($"normalize:{root}");
                    root = "steady";
                    return ValueTask.CompletedTask;
                },
                () =>
                {
                    events.Add("prefix-owned");
                    return ValueTask.CompletedTask;
                },
                async () =>
                {
                    events.Add("OnJoinCompleted");
                    root = "second-root";
                    completed.SetResult();
                    await continueCompletion.Task;
                },
                () =>
                {
                    events.Add("command44");
                    return ValueTask.CompletedTask;
                },
                () =>
                {
                    events.Add("handler");
                    return ValueTask.CompletedTask;
                }
            )
            .AsTask();
        await completed.Task;
        var atCompletion = events.ToArray();
        continueCompletion.SetResult();
        await activation;
        Assert.Equal(
            new[] { "normalize:first-root", "prefix-owned", "OnJoinCompleted" },
            atCompletion
        );
        Assert.Equal("second-root", root);
        Assert.Equal(
            new[]
            {
                "normalize:first-root",
                "prefix-owned",
                "OnJoinCompleted",
                "command44",
                "handler",
            },
            events
        );
    }

    [Fact]
    public async Task Actor_fifo_takes_Q_after_owned_P2_and_returns_its_initial_permit()
    {
        using var host = new ZLinkApplicationJobQueue(
            new(ZLinkApplicationJobQueueProfile.Balanced, 1, 1, 1)
        );
        var mailbox = new ZLinkActorSerialExecutor();
        var events = new System.Collections.Concurrent.ConcurrentQueue<string>();
        var firstStarted = Signal();
        var finishFirst = Signal();
        var secondOwned = Signal();
        ZLinkActorDurableRecord Record(string name, Action? owned = null)
        {
            ZLinkApplicationJobQueueLease? permit = null;
            return new(
                async _ =>
                {
                    using var invocation = ZLinkApplicationJobQueueInvocation.Enter(permit!);
                    await ZLinkApplicationJobQueueInvocation.ReleaseForHandlerStartAsync();
                    events.Enqueue(name);
                    if (name == "P1")
                    {
                        firstStarted.SetResult();
                        await finishFirst.Task;
                    }
                },
                async ct =>
                {
                    permit = await host.AcquireAsync(ct, ZLinkApplicationJobOrigin.Remote);
                },
                () => permit?.Dispose(),
                owned
            );
        }
        var prefix = mailbox.AdmitDurablePrefix(
            new[] { Record("P1"), Record("P2", () => secondOwned.SetResult()) }
        );
        await secondOwned.Task;
        await firstStarted.Task;
        using var initial = await host.AcquireAsync(default, ZLinkApplicationJobOrigin.Remote);
        initial.MarkQueued();
        async Task Ingress()
        {
            using var invocation = ZLinkApplicationJobQueueInvocation.Enter(initial);
            using var turn = await mailbox.EnterAsync(default);
            await ZLinkApplicationJobQueueInvocation.ReleaseForHandlerStartAsync();
            events.Enqueue("Q");
        }
        var q = Ingress();
        var permitsAfterQ = host.GetStatus().PermitsInUse;
        // Capture the synchronous ownership observation before releasing P1.
        // Cleanup lets the deliberately bad implementation finish as well.
        if (permitsAfterQ != 0)
            initial.Dispose();
        finishFirst.SetResult();
        await Task.WhenAll(prefix.Append(q));
        Assert.Equal(0UL, permitsAfterQ);
        Assert.Equal(new[] { "P1", "P2", "Q" }, events);
        Assert.Equal(0UL, host.GetStatus().PermitsInUse);
        Assert.Equal(0UL, host.GetStatus().CapacityWaiters);
    }

    // 03-spot-actor/05-spot-actor-membership §4: Join completion, OperationId,
    // optional reply and retry cursor live only in the current source and
    // target process. No Store journal or Store-driven completion replay
    // remains in the runtime.
    [Fact]
    public void Join_completion_has_no_durable_journal_or_store_replay()
    {
        var assembly = typeof(ZLinkActorRuntimeState).Assembly;
        Assert.Null(
            assembly.GetType(
                "Zlink.Framework.Runtime.Actors.ZLinkDeferredActorJoinCompletionJournal"
            )
        );
        var runtimeMethods = typeof(Zlink.Framework.Runtime.Host.ZLinkFrameworkRuntime)
            .GetMethods(
                System.Reflection.BindingFlags.Instance
                    | System.Reflection.BindingFlags.Static
                    | System.Reflection.BindingFlags.Public
                    | System.Reflection.BindingFlags.NonPublic
            )
            .Select(static method => method.Name)
            .ToHashSet(StringComparer.Ordinal);
        Assert.DoesNotContain("RecoverCanonicalRemoteJoinCompletionAsync", runtimeMethods);
        Assert.DoesNotContain("RunDeferredJoinCompletionRecoveryAsync", runtimeMethods);
        Assert.DoesNotContain("RecoverPublishedRelocationsAsync", runtimeMethods);
    }

    [Fact]
    public async Task Target_completion_waits_for_the_actor_mailbox_turn()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        state.BindNativeActorRef(
            new ZLinkBackendActorRef(RoutingId.From("node-target"), "actor-1", 7)
        );
        state.BindActorInstance(new TestActor("actor-1"));
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var order = new List<string>();

        var current = state
            .ExecuteLifecycleAsync(
                async _ =>
                {
                    order.Add("message");
                    entered.SetResult();
                    await release.Task;
                },
                CancellationToken.None
            )
            .AsTask();
        await entered.Task;
        var completion = state
            .ExecuteRelocationCompletionAsync(
                7,
                _ =>
                {
                    order.Add("completion");
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None
            )
            .AsTask();

        Assert.False(completion.IsCompleted);
        release.SetResult();
        await Task.WhenAll(current, completion);
        Assert.Equal(["message", "completion"], order);
    }
}
