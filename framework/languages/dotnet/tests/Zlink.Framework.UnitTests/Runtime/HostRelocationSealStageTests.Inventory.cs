using Zlink.Framework.LocationProvider;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class HostRelocationSealStageTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task SpotWide_rejects_missing_or_damaged_inventory_before_restore(bool damaged)
    {
        var trace = new RelocationBehaviorTrace();
        var targetFactoryCalls = 0;
        trace.ConfigureTarget = context =>
        {
            if (context.SpotId == "zone-2")
                Interlocked.Increment(ref targetFactoryCalls);
        };
        InventoryFaultStore? injected = null;

        await RunBoundMemberRelocationAsync(
            trace,
            async context =>
            {
                var factoryCallsBeforeRelocation = Volatile.Read(ref targetFactoryCalls);
                var result = await context.StartRelocation().WaitAsync(TimeSpan.FromSeconds(15));
                Assert.NotEqual(ZLinkFrameworkRelocationOutcome.Relocated, result.Outcome);
                Assert.Equal(ZLinkFrameworkRelocationReason.StateIncompatible, result.Reason);
                Assert.Equal(factoryCallsBeforeRelocation, Volatile.Read(ref targetFactoryCalls));
                Assert.DoesNotContain("targetStateRestoring", trace.Events);
                Assert.NotNull(injected!.FaultedKey);
                var key = injected.FaultedKey!;
                var inventoryPrefix = key[..key.IndexOf(":participant:", StringComparison.Ordinal)];
                var remaining = Assert.IsType<ZLinkStoreScanResult.Page>(
                    await injected.ScanAsync(
                        new ZLinkStoreScanRequest(inventoryPrefix + ":", null, 256)
                    )
                );
                Assert.Empty(remaining.Value.Items);
            },
            (inner, _, _) => injected = new InventoryFaultStore(inner, damaged)
        );
    }

    private sealed class InventoryFaultStore(IZLinkLocationStore inner, bool damaged)
        : IZLinkLocationStore
    {
        internal string? FaultedKey { get; private set; }

        public async ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        )
        {
            var result = await inner.ReadAsync(key, cancellationToken);
            if (
                result is not ZLinkStoreReadResult.Found found
                || !key.Value.StartsWith("zlink:v11:aggregate-inventory:", StringComparison.Ordinal)
                || !key.Value.EndsWith(":participant:0:authorityPayload", StringComparison.Ordinal)
            )
                return result;
            FaultedKey = key.Value;
            return damaged
                ? new ZLinkStoreReadResult.Found(found.Value with { Bytes = new byte[] { 255 } })
                : new ZLinkStoreReadResult.Missing(found.Value.StoreNow);
        }

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        ) => inner.WriteAsync(request, cancellationToken);

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        ) => inner.ScanAsync(request, cancellationToken);
    }
}
