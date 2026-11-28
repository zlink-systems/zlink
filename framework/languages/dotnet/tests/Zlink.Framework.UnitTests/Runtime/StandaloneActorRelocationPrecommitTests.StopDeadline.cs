using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed partial class StandaloneActorRelocationPrecommitTests
{
    // Host relocation §14 (#5797): a stop deadline that passes while the Preserve
    // fence cannot be decided ends the settlement. The source changes nothing in
    // the Store and never calls it again for that unit.
    [Fact]
    public async Task Source_settlement_stops_calling_the_Store_at_the_stop_deadline()
    {
        var inner = new ZLinkInMemoryLocationStore();
        var source = await PrepareCapturedSourceAsync(inner);
        var store = new UndecidedPreserveStore(inner);
        using var restoreDeadline = new CancellationTokenSource();
        using var stopDeadline = new CancellationTokenSource();
        restoreDeadline.Cancel();

        var settlement = ZLinkStandaloneActorRelocationRuntime
            .SettleSourceAsync(
                store,
                source.Key,
                source.Steady,
                source.RelocationId,
                source.Target,
                TimeSpan.MaxValue,
                static () => true,
                static _ => ValueTask.CompletedTask,
                TimeSpan.FromMilliseconds(5),
                restoreDeadline.Token,
                ReadyRoute,
                stopDeadline.Token
            )
            .AsTask();

        // The Preserve fence is retried while the stop deadline is not reached.
        await store.Retried.Task.WaitAsync(ReadBarrierTimeout);
        stopDeadline.Cancel();
        await Assert.ThrowsAsync<ZLinkAuthorityUnsettledAtStopException>(async () =>
            await settlement.WaitAsync(ReadBarrierTimeout)
        );

        var callsAtStop = store.Calls;
        await Task.Delay(TimeSpan.FromMilliseconds(100));
        Assert.Equal(callsAtStop, store.Calls);
    }

    // Host relocation §14 (#5797): a Store failure that arrives after the stop
    // deadline is not treated as indeterminate and retried; the settlement ends.
    [Fact]
    public async Task Preserve_failure_after_the_stop_deadline_ends_the_settlement()
    {
        var inner = new ZLinkInMemoryLocationStore();
        var source = await PrepareCapturedSourceAsync(inner);
        using var restoreDeadline = new CancellationTokenSource();
        using var stopDeadline = new CancellationTokenSource();
        restoreDeadline.Cancel();
        var store = new StopThenFailPreserveStore(inner, stopDeadline);

        await Assert.ThrowsAsync<ZLinkAuthorityUnsettledAtStopException>(async () =>
            await ZLinkStandaloneActorRelocationRuntime
                .SettleSourceAsync(
                    store,
                    source.Key,
                    source.Steady,
                    source.RelocationId,
                    source.Target,
                    TimeSpan.MaxValue,
                    static () => true,
                    static _ => ValueTask.CompletedTask,
                    TimeSpan.FromMilliseconds(5),
                    restoreDeadline.Token,
                    ReadyRoute,
                    stopDeadline.Token
                )
                .AsTask()
                .WaitAsync(ReadBarrierTimeout)
        );
        Assert.Equal(1, store.Preserves);
    }

    private sealed class StopThenFailPreserveStore(
        IZLinkLocationRepository inner,
        CancellationTokenSource stopDeadline
    ) : ZLinkLocationStoreTestDouble
    {
        private int _preserves;

        internal int Preserves => Volatile.Read(ref _preserves);

        public override ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
            ZLinkAuthorityKey key,
            CancellationToken cancellationToken = default
        ) => inner.ReadAuthorityAsync(key, cancellationToken);

        public override ValueTask<ZLinkAuthorityCompareExchangeResult> CompareExchangeAuthorityAsync(
            ZLinkAuthorityKey key,
            string expectedStoreVersion,
            ZLinkAuthorityMutation mutation,
            CancellationToken cancellationToken = default
        )
        {
            Interlocked.Increment(ref _preserves);
            stopDeadline.Cancel();
            throw new IOException("The Store failed as the stop deadline passed.");
        }
    }

    private sealed class UndecidedPreserveStore(IZLinkLocationRepository inner)
        : ZLinkLocationStoreTestDouble
    {
        private int _calls;

        internal int Calls => Volatile.Read(ref _calls);

        internal TaskCompletionSource Retried { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public override ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
            ZLinkAuthorityKey key,
            CancellationToken cancellationToken = default
        )
        {
            if (Interlocked.Increment(ref _calls) > 6)
                Retried.TrySetResult();
            return inner.ReadAuthorityAsync(key, cancellationToken);
        }

        public override ValueTask<ZLinkAuthorityCompareExchangeResult> CompareExchangeAuthorityAsync(
            ZLinkAuthorityKey key,
            string expectedStoreVersion,
            ZLinkAuthorityMutation mutation,
            CancellationToken cancellationToken = default
        )
        {
            Interlocked.Increment(ref _calls);
            throw new IOException("The Preserve response was lost.");
        }
    }
}
