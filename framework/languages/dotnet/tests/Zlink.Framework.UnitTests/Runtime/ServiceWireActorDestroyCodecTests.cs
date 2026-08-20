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
}
