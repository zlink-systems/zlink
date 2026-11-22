using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed partial class ProviderLocationRepositoryAuthorityTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task OwnerTokenReleaseWaitSharesAcrossRowsAndSeparatesGenerations(bool inMemory)
    {
        var time = new ManualTimeProvider();
        var provider = new ZLinkInMemoryProviderLocationStore(time);
        IZLinkLocationRepository CreateRepository() =>
            inMemory
                ? new ZLinkInMemoryLocationStore(time)
                : new ZLinkProviderLocationRepository(provider, time);

        async Task<ZLinkMeshNodeDescriptor> Publish(
            IZLinkLocationRepository repository,
            string rid,
            ZLinkLocationOwnerToken owner
        )
        {
            var descriptor = Descriptor(rid, owner);
            await PublishAsync(repository, descriptor, ZLinkLocationWriteIntent.NewClaim);
            return descriptor;
        }

        var repository = CreateRepository();
        var (target, targetDescriptor) = await SeedAsync(repository);
        var first = Request("first");
        var second = Request("second");
        var newer = Request("new-generation");
        var untouched = Request("untouched");

        await AssertWaiting(repository, first);
        time.Advance(TimeSpan.FromSeconds(15) - TimeSpan.FromTicks(1));
        await AssertWaiting(repository, first);
        time.Advance(TimeSpan.FromTicks(1));
        Assert.IsType<ZLinkObjectReserveResult.Reserved>(await repository.ReserveAsync(first));
        Assert.IsType<ZLinkObjectReserveResult.Reserved>(await repository.ReserveAsync(second));
        await AssertWaiting(repository, newer);

        var restarted = CreateRepository();
        if (inMemory)
            _ = await SeedAsync(restarted);
        await AssertWaiting(restarted, untouched);

        time.Advance(TimeSpan.FromSeconds(15));
        Assert.IsType<ZLinkObjectReserveResult.Reserved>(await repository.ReserveAsync(newer));
        Assert.IsType<ZLinkObjectReserveResult.Reserved>(await restarted.ReserveAsync(untouched));

        ZLinkObjectReservationRequest Request(string actorId) =>
            Reservation(actorId, targetDescriptor, target) with
            {
                ObjectRelocationPolicy = ZLinkObjectRelocationRegistration.DisabledPolicy,
            };

        async Task<(ZLinkLocationOwnerToken, ZLinkMeshNodeDescriptor)> SeedAsync(
            IZLinkLocationRepository observer
        )
        {
            var old = await ClaimAsync(observer, "release-owner");
            var descriptor = await Publish(observer, "first-node", old);
            await CreateReadyAsync(observer, descriptor, old, "first", 1);
            await CreateReadyAsync(observer, descriptor, old, "second", 2);
            await CreateReadyAsync(observer, descriptor, old, "untouched", 3);
            Assert.Equal(
                ZLinkOwnerLeaseReleaseResult.Released,
                await observer.ReleaseOwnerLeaseAsync(old)
            );

            var next = await ClaimAsync(observer, old.OwnerId);
            Assert.Equal(old.LeaseGeneration + 1, next.LeaseGeneration);
            var nextDescriptor = await Publish(observer, "next-node", next);
            await CreateReadyAsync(observer, nextDescriptor, next, "new-generation", 4);
            Assert.Equal(
                ZLinkOwnerLeaseReleaseResult.Released,
                await observer.ReleaseOwnerLeaseAsync(next)
            );

            var live = await ClaimAsync(observer, "release-target");
            return (live, await Publish(observer, "target-node", live));
        }

        async Task CreateReadyAsync(
            IZLinkLocationRepository observer,
            ZLinkMeshNodeDescriptor descriptor,
            ZLinkLocationOwnerToken owner,
            string id,
            ulong operationId
        )
        {
            var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
                await observer.ReserveAsync(Reservation(id, descriptor, owner))
            );
            _ = await observer.CompleteCreationAsync(
                reserved.Reservation,
                new ZLinkObjectCreationCompletion.Created(
                    ActorPayload(id, descriptor, owner, ZLinkActorAuthorityState.Ready),
                    new ZLinkCreationTerminalPublication(
                        new ZLinkCreationOperationId(descriptor.Rid, 1, 1, operationId),
                        new byte[] { 1 },
                        time.GetUtcNow().AddMinutes(5)
                    )
                )
            );
        }

        static async Task AssertWaiting(
            IZLinkLocationRepository observer,
            ZLinkObjectReservationRequest request
        )
        {
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await observer.ReserveAsync(request)
            );
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, failure.Kind);
        }
    }
}
