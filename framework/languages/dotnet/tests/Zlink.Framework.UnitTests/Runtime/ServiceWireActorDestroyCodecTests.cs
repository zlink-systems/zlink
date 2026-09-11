using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.UnitTests;

// service-wire-v1.schema.json command 27 actorDestroy: correlation followed by
// actor-route-fence (actor-ref, targetNodeRid, targetNodeGeneration,
// expectedAuthorityOwnerGeneration, expectedOwnerLeaseGeneration).
public sealed class ServiceWireActorDestroyCodecTests
{
    [Fact]
    public void Generated_actorDestroy27_vector_is_byte_identical_and_round_trips()
    {
        var targetNodeRid = RoutingId.From(new byte[] { 1, 2, 3 });
        var operation = new ActorDestroyOperation(
            42,
            new ActorRef("actor-27", 17, "mesh", targetNodeRid),
            targetNodeRid,
            19,
            23,
            29);
        var generated = ServiceWirePilotCodec.EncodeActorDestroy27(
            new ServiceWirePilotCodec.ActorDestroy27(
                operation.Correlation,
                new ServiceWirePilotCodec.Fence(
                    operation.Actor.ActorId,
                    operation.Actor.ObjectGeneration,
                    operation.TargetNodeRid.ToBytes().ToArray(),
                    operation.TargetNodeGeneration,
                    operation.AuthorityOwnerGeneration,
                    operation.OwnerLeaseGeneration)));

        var encoded = ZLinkServiceWireCodec.EncodeActorDestroy(operation);

        Assert.Equal(generated, encoded);
        Assert.True(ZLinkServiceWireCodec.TryDecodeActorDestroy(
            generated, "mesh", out var decoded, out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(operation, decoded.Operation);
        Assert.Equal(29UL, decoded.Operation.OwnerLeaseGeneration);
    }

    [Theory]
    [InlineData("valid", "None")]
    [InlineData("truncated-prefix", "TruncatedField")]
    [InlineData("magic", "InvalidMagic")]
    [InlineData("version", "UnsupportedVersion")]
    [InlineData("command", "UnknownCommand")]
    [InlineData("flags", "ForbiddenFlag")]
    [InlineData("truncated-body", "TruncatedField")]
    [InlineData("trailing-body", "TrailingByte")]
    public void DecodedPrefix_PreservesActorDestroySchemaValidation(
        string vector,
        string expectedError)
    {
        // Canonical command 27: correlation 42, actor-27 generation 17,
        // RID 01:02:03, node/authority/lease generations 19/23/29.
        var bytes = Convert.FromHexString(
            "5A4D011B00000000000000002A086163746F722D3237"
            + "0000000000000011030102030000000000000013"
            + "0000000000000017000000000000001D");
        switch (vector)
        {
            case "truncated-prefix": bytes = bytes[..4]; break;
            case "magic": bytes[0] = 0; break;
            case "version": bytes[2] = 2; break;
            case "command": bytes[3] = 255; break;
            case "flags": bytes[4] = 1; break;
            case "truncated-body": bytes = bytes[..^1]; break;
            case "trailing-body": bytes = [.. bytes, 0]; break;
        }

        var decoded = default(ZLinkServiceWireCodec.ActorDestroyOperationRecord);
        var accepted = ZLinkServiceWireCodec.TryDecodePrefix(
            bytes, out var command, out var flags, out var error)
            && ZLinkServiceWireCodec.TryDecodeActorDestroy(
                bytes, command, flags, "mesh", out decoded, out error);

        Assert.Equal(expectedError, error.ToString());
        Assert.Equal(vector == "valid", accepted);
        if (!accepted)
            return;
        Assert.Equal(42UL, decoded.Operation.Correlation);
        Assert.Equal("actor-27", decoded.Operation.Actor.ActorId);
        Assert.Equal(17UL, decoded.Operation.Actor.ObjectGeneration);
        Assert.Equal(RoutingId.From(new byte[] { 1, 2, 3 }), decoded.Operation.TargetNodeRid);
        Assert.Equal(19UL, decoded.Operation.TargetNodeGeneration);
        Assert.Equal(23UL, decoded.Operation.AuthorityOwnerGeneration);
        Assert.Equal(29UL, decoded.Operation.OwnerLeaseGeneration);
    }
}
