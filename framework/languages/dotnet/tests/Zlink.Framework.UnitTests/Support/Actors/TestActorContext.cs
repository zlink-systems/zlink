using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;

namespace Zlink.Framework.UnitTests;

internal sealed class TestActorContext(
    string actorId,
    ulong objectGeneration = 1,
    string meshName = "test-mesh",
    string? spotId = null) : IZLinkActorContext
{
    public string ActorId { get; } = actorId;

    public ulong ObjectGeneration { get; } = objectGeneration;

    public string MeshName { get; } = meshName;

    public string? SpotId { get; } = spotId;

    public IZLinkBoundSession BoundSession =>
        throw new NotSupportedException("This test Context does not provide a bound session.");

    public IZLinkActorJoinSpotCall JoinSpot(string spotId, ZLinkMessage request) =>
        throw new NotSupportedException("This test Context does not provide Actor Join.");

    public IZLinkActorJoinEntrySpotCall JoinEntrySpot(ZLinkMessage request) =>
        throw new NotSupportedException("This test Context does not provide Entry Spot Join.");
}
