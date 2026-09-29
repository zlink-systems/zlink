using System.Collections.Concurrent;
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

    // (a) Work the queue accepted before the Close seal runs before Close
    // continues past step 2, even when it had not started at seal time.
    [Fact]
    public async Task Close_seal_processes_work_accepted_before_the_seal()
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
                await executor.AwaitCloseDrainAsync(ct).ConfigureAwait(false);
                order.Enqueue("drained");
                return true;
            }
        );
        Assert.NotNull(close);
        releaseFirst.TrySetResult();

        Assert.True(await close.WaitAsync(Wait));
        await secondCompletion.WaitAsync(Wait);
        Assert.Equal(new[] { "first", "second", "drained" }, order.ToArray());
    }

    // (b) After the Close seal, new work of every entry receives the seal's
    // result: Closing (Rejected), not a silent drop or ShuttingDown.
    [Fact]
    public async Task Close_seal_answers_new_admission_with_closing()
    {
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var executor = CreateExecutor(errorSink);
        var close = executor.PostCloseLifecycle(
            async (_, ct) =>
            {
                await executor.AwaitCloseDrainAsync(ct).ConfigureAwait(false);
                return true;
            }
        );
        Assert.NotNull(close);
        Assert.True(await close.WaitAsync(Wait));

        Assert.Equal(
            ZLinkAcceptedWorkAdmission.Closing,
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

    // (b) End to end: after Closing commit a request resolves a Closing
    // authority, and the resolver ends it Rejected (spec 06 §9, 08-routing).
    [Fact]
    public async Task Request_after_closing_commit_is_rejected()
    {
        await using var host = await SpotCloseHost.StartAsync();
        var spot = await host.CreateSpotAsync();
        host.State.HoldOnClosing = true;
        var close = host.CloseAsync(spot);
        await host.State.OnClosingEntered.Task.WaitAsync(Wait);

        // The resolver observes the Closing authority and ends with the §9
        // terminal kind; the request below takes the same resolver path.
        var resolved = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await host.Runtime.ResolveSpotHandleAsync(spot.SpotId, CancellationToken.None)
        );
        Assert.Equal(ZLinkFrameworkErrorKind.Rejected, resolved.Kind);
        var result = await host.RequestAsync(spot.SpotId);

        Assert.Equal(
            ZLinkFrameworkErrorKind.Rejected,
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
