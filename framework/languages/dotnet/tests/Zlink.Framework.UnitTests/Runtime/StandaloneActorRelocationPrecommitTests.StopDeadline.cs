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
