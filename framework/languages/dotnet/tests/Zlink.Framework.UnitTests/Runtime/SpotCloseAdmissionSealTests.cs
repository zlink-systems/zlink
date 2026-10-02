using System.Collections.Concurrent;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

/// <summary>
/// Spot Close and host drain admission (spec 06-spot-address-messaging §7 step 2
/// and §9): the Spot execution queue's admission seal decides Closing and the
/// host admission gate decides Draining.
/// </summary>
public sealed class SpotCloseAdmissionSealTests
{
    private static readonly TimeSpan Wait = TimeSpan.FromSeconds(10);

    [Fact]
    public async Task Failed_close_CAS_preserves_message_admitted_during_the_attempt()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(errorSink);
        var boundary = Signal();
        var releaseCAS = Signal();
        var messageCalls = 0;
        var close = executor.PostCloseLifecycle(
            async (_, ct) =>
            {
                await executor.BeginCloseBoundaryAsync(ct);
                boundary.TrySetResult();
                await ZLinkSerialTurn.Current!.YieldFrameworkCallAsync(
                    _ => new ValueTask(releaseCAS.Task),
                    ct
                );
                executor.AbortCloseBoundary();
                return false;
            }
        );
        await boundary.Task.WaitAsync(Wait);
        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            executor.QueueAccepted(
                new byte[] { 1 },
                (_, _) =>
                {
                    Interlocked.Increment(ref messageCalls);
                    return ValueTask.CompletedTask;
                },
                static () => { },
                out var message
            )
        );
        Assert.False(message.IsCompleted);
        releaseCAS.TrySetResult();
        Assert.False(await close!.WaitAsync(Wait));
        await message.WaitAsync(Wait);
        Assert.Equal(1, messageCalls);
    }

    [Fact]
    public async Task Close_preserves_started_message_continuation()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(errorSink);
        var started = Signal();
        var release = Signal();
        var closeStarted = Signal();
        var order = new ConcurrentQueue<string>();
        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            executor.QueueAccepted(
                new byte[] { 1 },
                async (_, ct) =>
                {
                    started.TrySetResult();
                    await ZLinkSerialTurn.Current!.YieldFrameworkCallAsync(
                        _ => new ValueTask(release.Task),
                        ct
                    );
                    order.Enqueue("started-message-completed");
                },
                static () => { },
                out var application
            )
        );
        await started.Task.WaitAsync(Wait);
        var close = executor.PostCloseLifecycle(
            async (_, ct) =>
            {
                await executor.BeginCloseBoundaryAsync(ct);
                closeStarted.TrySetResult();
                await executor.AwaitStartedCloseCallsAsync(ct);
                order.Enqueue("close");
                return true;
            }
        );
        await closeStarted.Task.WaitAsync(Wait);
        Assert.False(close!.IsCompleted);
        release.TrySetResult();
        Assert.True(await close.WaitAsync(Wait));
        await application.WaitAsync(Wait);
        Assert.Equal(new[] { "started-message-completed", "close" }, order.ToArray());
    }

    [Fact]
    public async Task Close_cleans_pending_timer_without_running_its_handler()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(errorSink);
        var started = Signal();
        var release = Signal();
        Assert.True(
            executor.Queue(
                async (_, _) =>
                {
                    started.TrySetResult();
                    await release.Task;
                }
            )
        );
        await started.Task.WaitAsync(Wait);
        var timerCalls = 0;
        var timer = executor
            .ExecuteTimerAsync(
                "pending-timer",
                (_, _, _) =>
                {
                    Interlocked.Increment(ref timerCalls);
                    return ValueTask.CompletedTask;
                },
                0,
                CancellationToken.None
            )
            .AsTask();
        var close = executor.PostCloseLifecycle(
            async (_, ct) =>
            {
                await executor.BeginCloseBoundaryAsync(ct);
                await executor.AwaitStartedCloseCallsAsync(ct);
                return true;
            }
        );
        release.TrySetResult();
        Assert.True(await close!.WaitAsync(Wait));
        await timer.WaitAsync(Wait);
        Assert.Equal(0, timerCalls);
    }

    // Spot messaging §7: a lifecycle Close precedes application messages
    // whose handler turn has not started.
    [Fact]
    public async Task Close_item_precedes_unstarted_application_work()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(errorSink);
        var order = new ConcurrentQueue<string>();
        var firstStarted = Signal();
        var releaseFirst = Signal();

        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            executor.QueueAccepted(
                new byte[] { 1 },
                async (_, _) =>
                {
                    firstStarted.TrySetResult();
                    await releaseFirst.Task.ConfigureAwait(false);
                    order.Enqueue("first");
                },
                static () => { },
                out _
            )
        );
        await firstStarted.Task.WaitAsync(Wait);
        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            executor.QueueAccepted(
                new byte[] { 2 },
                (_, _) =>
                {
                    order.Enqueue("second");
                    return ValueTask.CompletedTask;
                },
                static () => { },
                out var secondCompletion
            )
        );

        var close = executor.PostCloseLifecycle(
            async (_, ct) =>
            {
                await executor.BeginCloseBoundaryAsync(ct).ConfigureAwait(false);
                order.Enqueue("drained");
                return true;
            }
        );
        Assert.NotNull(close);
        releaseFirst.TrySetResult();

        Assert.True(await close.WaitAsync(Wait));
        await secondCompletion.WaitAsync(Wait);
        Assert.Equal(new[] { "first", "drained", "second" }, order.ToArray());
    }

    // Application message records remain admitted to the existing FIFO;
    // timer and actor admission remain sealed for the old incarnation.
    [Fact]
    public async Task Close_preserves_message_admission_and_seals_old_timer_and_actor_turns()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(errorSink);
        var close = executor.PostCloseLifecycle(
            async (_, ct) =>
            {
                await executor.BeginCloseBoundaryAsync(ct).ConfigureAwait(false);
                return true;
            }
        );
        Assert.NotNull(close);
        Assert.True(await close.WaitAsync(Wait));

        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            executor.QueueAccepted(
                new byte[] { 1 },
                static (_, _) => ValueTask.CompletedTask,
                static () => { },
                out _
            )
        );
        var spot = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            executor
                .ExecuteAsync(static (_, _) => ValueTask.CompletedTask, CancellationToken.None)
                .AsTask()
        );
        Assert.Equal(ZLinkFrameworkErrorKind.Rejected, spot.Kind);
        var timer = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            executor
                .ExecuteTimerAsync(
                    "timer",
                    static (_, _, _) => ValueTask.CompletedTask,
                    0,
                    CancellationToken.None
                )
                .AsTask()
        );
        Assert.Equal(ZLinkFrameworkErrorKind.Rejected, timer.Kind);
        var actor = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            executor
                .ExecuteActorAsync(
                    "actor",
                    static (_, _, _) => ValueTask.CompletedTask,
                    0,
                    CancellationToken.None
                )
                .AsTask()
        );
        Assert.Equal(ZLinkFrameworkErrorKind.Rejected, actor.Kind);
    }

    // Closing preserves its owner route. A request without creation intent
    // completes NotFound before the held OnClosing callback is released.
    [Fact]
    public async Task Request_without_intent_after_closing_commit_is_not_found()
    {
        await using var host = await SpotCloseHost.StartAsync();
        var spot = await host.CreateSpotAsync();
        host.State.HoldOnClosing = true;
        var close = host.CloseAsync(spot);
        await host.State.OnClosingEntered.Task.WaitAsync(Wait);

        // The resolver observes the Closing authority and ends with the §9
        // terminal kind; the request below takes the same resolver path.
        var resolved = await host.Runtime.ResolveSpotHandleAsync(
            spot.SpotId,
            CancellationToken.None
        );
        Assert.NotNull(resolved);
        Assert.Equal(spot.NodeRid, resolved.Snapshot.NodeRid);
        var result = await host.RequestAsync(spot.SpotId);

        Assert.Equal(
            ZLinkFrameworkErrorKind.NotFound,
            Assert.IsType<ZLinkFrameworkException>(result).Kind
        );
        host.State.ReleaseOnClosing.TrySetResult();
        Assert.Equal(true, await close.WaitAsync(Wait));
    }

    // (c) Draining is decided by the host admission gate: a new Spot on a
    // draining host is ShuttingDown.
    [Fact]
    public async Task Draining_host_answers_new_spot_admission_with_shutting_down()
    {
        await using var host = await SpotCloseHost.StartAsync();
        Assert.True(host.Runtime.DrainAdmission.BeginDrain(ZLinkDrainOwner.Shutdown));

        var create = await Assert.ThrowsAsync<ZLinkFrameworkException>(host.CreateSpotAsync);
        Assert.Equal(ZLinkFrameworkErrorKind.ShuttingDown, create.Kind);
    }

    // (d) A Close that ends false because of membership leaves admission as it
    // was: a message that arrived during the attempt is processed.
    [Fact]
    public async Task Close_false_for_membership_keeps_admission_for_messages_that_arrived_meanwhile()
    {
        await using var host = await SpotCloseHost.StartAsync();
        var spot = await host.CreateSpotAsync();
        host.State.HoldJoin = true;
        await host.JoinActorAsync(spot.SpotId);
        await host.State.JoinEntered.Task.WaitAsync(Wait);

        var close = host.CloseAsync(spot);
        var request = host.RequestAsync(spot.SpotId);
        host.State.ReleaseJoin.TrySetResult();

        Assert.Equal(false, await close.WaitAsync(Wait));
        Assert.Equal("handlerReply", await request.WaitAsync(Wait));
        Assert.Equal("open", await host.AdmissionAsync(spot.SpotId));
    }

    // (e) Ingress after a relocation seal is held, not rejected.
    [Fact]
    public async Task Relocation_seal_holds_new_ingress()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(errorSink);
        Assert.True(executor.TrySealRelocation(out var seal));

        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Accepted,
            executor.QueueAccepted(
                new byte[] { 7 },
                static (_, _) => ValueTask.CompletedTask,
                static () => { },
                out _
            )
        );
        Assert.True(executor.TryFreezeRelocationIngress(seal, out var held));
        Assert.Single(held);
    }

    private static ZLinkSpotSerialExecutor CreateExecutor(IZLinkRuntimeFailureReporter errorSink) =>
        new(
            null!,
            static () => false,
            CancellationToken.None,
            errorSink,
            executionMode: ZLinkUserSpotExecutionMode.SpotWide
        );

    private static TaskCompletionSource Signal() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);
}
