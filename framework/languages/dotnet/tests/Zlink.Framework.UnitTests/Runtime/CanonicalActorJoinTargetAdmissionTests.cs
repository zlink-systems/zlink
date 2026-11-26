using Zlink.Framework.Runtime.Locations;
using static Zlink.Framework.UnitTests.CanonicalActorJoinWireAdmissionNegativeTests;

namespace Zlink.Framework.UnitTests;

public sealed class CanonicalActorJoinTargetAdmissionTests
{
    [Fact]
    public async Task Entry_spot_is_admitted_without_a_user_spot_authority_row()
    {
        await using var fixture = await WireAdmissionFixture.CreateAsync(includeEntrySpot: true);
        var request = fixture.CreateEntryRequest();
        var store = fixture.Runtime.Registration.Locations.ResolveStore()!;
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(
            await store.ReadAuthorityAsync(
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(request.TargetSpotId)
            )
        );

        var reply = await fixture.Runtime.AdmitCanonicalActorJoinAsync(
            request.TargetSpotId,
            request,
            payload: null
        );

        Assert.True(reply.Accepted);
    }

    [Theory]
    [InlineData(EntryFenceMismatch.SpotGeneration)]
    [InlineData(EntryFenceMismatch.NodeGeneration)]
    [InlineData(EntryFenceMismatch.AuthorityOwnerGeneration)]
    [InlineData(EntryFenceMismatch.OwnerLeaseGeneration)]
    public async Task Entry_spot_rejects_mismatched_target_fence(EntryFenceMismatch mismatch)
    {
        await using var fixture = await WireAdmissionFixture.CreateAsync(includeEntrySpot: true);
        var request = fixture.CreateEntryRequest();
        request = mismatch switch
        {
            EntryFenceMismatch.SpotGeneration => request with
            {
                TargetSpotGeneration = request.TargetSpotGeneration ^ 1,
            },
            EntryFenceMismatch.NodeGeneration => request with
            {
                TargetNodeGeneration = request.TargetNodeGeneration ^ 1,
            },
            EntryFenceMismatch.AuthorityOwnerGeneration => request with
            {
                TargetAuthorityOwnerGeneration = request.TargetAuthorityOwnerGeneration ^ 1,
            },
            EntryFenceMismatch.OwnerLeaseGeneration => request with
            {
                TargetOwnerLeaseGeneration = request.TargetOwnerLeaseGeneration + 1,
            },
            _ => throw new ArgumentOutOfRangeException(nameof(mismatch)),
        };

        var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            fixture
                .Runtime.AdmitCanonicalActorJoinAsync(request.TargetSpotId, request, null)
                .AsTask()
        );

        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, failure.Kind);
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task User_spot_requires_matching_authority_lease(bool matches)
    {
        await using var fixture = await WireAdmissionFixture.CreateAsync();
        var request = fixture.CreateRequest();
        if (matches)
        {
            var reply = await fixture.Runtime.AdmitCanonicalActorJoinAsync(
                request.TargetSpotId,
                request,
                payload: null
            );
            Assert.True(reply.Accepted);
        }
        else
        {
            request = request with
            {
                TargetOwnerLeaseGeneration = request.TargetOwnerLeaseGeneration + 1,
            };
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                fixture
                    .Runtime.AdmitCanonicalActorJoinAsync(request.TargetSpotId, request, null)
                    .AsTask()
            );
            Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, failure.Kind);
        }
    }

    public enum EntryFenceMismatch
    {
        SpotGeneration,
        NodeGeneration,
        AuthorityOwnerGeneration,
        OwnerLeaseGeneration,
    }
}
