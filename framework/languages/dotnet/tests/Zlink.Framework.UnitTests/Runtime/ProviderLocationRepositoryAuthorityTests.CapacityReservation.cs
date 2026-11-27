using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed partial class ProviderLocationRepositoryAuthorityTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ReservationRechecksCapacityOnlyIfAuthorityStillMatches(bool changeDescriptor)
    {
        var inner = new ZLinkInMemoryProviderLocationStore();
        var provider = new InspectAuthorityBatchLocationStore(inner);
        var repository = new ZLinkProviderLocationRepository(provider);
        var owner = await ClaimAsync(repository, "capacity-conflict-owner");
        var descriptor = Descriptor("capacity-conflict", owner);
        await repository.UpdateMeshNodeAsync(descriptor, ZLinkLocationWriteIntent.NewClaim);
        await SeedCounterAsync(
            inner,
            CapacityKey(descriptor),
            descriptor.Capacity.Actors.Limit.ToString(
                System.Globalization.CultureInfo.InvariantCulture
            )
        );

        var capacityOnlyChecks = 0;
        provider.Inspect = async batch =>
        {
            if (!batch.Conditions.Any(condition => condition is ZLinkStoreCondition.SumAtMost))
                return;
            if (batch.Mutations.Count == 0)
                capacityOnlyChecks++;
            else if (changeDescriptor)
                await repository.UpdateMeshNodeAsync(
                    descriptor with
                    {
                        DescriptorRevision = descriptor.DescriptorRevision + 1,
                        PlacementWeight = 0,
                    },
                    ZLinkLocationWriteIntent.Renew
                );
        };

        var result = await repository.ReserveAsync(
            Reservation("actor:capacity-conflict", descriptor, owner)
        );

        if (changeDescriptor)
        {
            var conflict = Assert.IsType<ZLinkObjectReserveResult.Conflict>(result);
            Assert.IsType<ZLinkAuthorityReadResult.Missing>(conflict.Current);
            Assert.Equal(0, capacityOnlyChecks);
        }
        else
        {
            Assert.IsType<ZLinkObjectReserveResult.PlacementCapacityExhausted>(result);
            Assert.Equal(1, capacityOnlyChecks);
        }
    }
}
