using Systems.Zlink;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

/// <summary>
/// Cross-language fixed-vector conformance for the actor ZLAU payload. This
/// hex vector and field set are shared verbatim with
/// framework/languages/node/test/contract/actor-authority-payload.test.js
/// ("Actor authority payload matches the .NET byte layout and decodes
/// byte-exact fields"), completing the 4-language byte-exact match for the
/// actor authority wire format (node, java, cpp, dotnet).
/// </summary>
public sealed class ActorAuthorityPayloadFixedVectorTests
{
    private const string ExpectedHex =
        "5a4c4155010000000000340001001001410142010143"
        + "0000000000000002010144000000000000000301450146"
        + "000000000000000400000000000000000000b2374797";

    private static ZLinkActorAuthorityPayload FixedVector() => new(
        State: ZLinkActorAuthorityState.Ready,
        StableType: "A",
        ActorId: "B",
        CurrentSpotId: "C",
        CurrentSpotGeneration: 2,
        CurrentSpotKind: ZLinkSpotKind.Entry,
        OwnerId: "D",
        OwnerLeaseGeneration: 3,
        MeshName: "E",
        NodeRid: RoutingId.From("F"),
        NodeGeneration: 4);

    [Fact]
    public void ActorAuthorityPayload_fixed_vector_encodes_byte_exactly()
    {
        var encoded = ZLinkActorAuthorityPayloadCodec.Encode(FixedVector());

        Assert.Equal(ExpectedHex, Convert.ToHexString(encoded).ToLowerInvariant());
    }

    [Fact]
    public void ActorAuthorityPayload_fixed_vector_decodes_byte_exact_fields()
    {
        var encoded = Convert.FromHexString(ExpectedHex);

        Assert.True(ZLinkActorAuthorityPayloadCodec.TryDecodeDirect(
            encoded,
            out var decoded));
        Assert.Equal(FixedVector(), decoded);
    }

    [Fact]
    public void ActorAuthorityPayload_fixed_vector_round_trips_byte_exactly()
    {
        var encoded = ZLinkActorAuthorityPayloadCodec.Encode(FixedVector());

        Assert.True(ZLinkActorAuthorityPayloadCodec.TryDecodeDirect(
            encoded,
            out var decoded));
        Assert.Equal(FixedVector(), decoded);
        Assert.Equal(
            ExpectedHex,
            Convert.ToHexString(ZLinkActorAuthorityPayloadCodec.Encode(decoded))
                .ToLowerInvariant());
    }
}
