using System;
using Systems.Zlink;
using Zlink.Framework.Runtime.Service;
using Systems.Zlink.Framework.Runtime.Protocol;

static string Hex(byte[] b) => Convert.ToHexString(b).ToLowerInvariant();

var allEqual = true;
void Check(string name, byte[] hand, byte[] gen)
{
    var handHex = Hex(hand);
    var genHex = Hex(gen);
    var equal = handHex == genHex;
    if (!equal) allEqual = false;
    Console.WriteLine($"{name}: {(equal ? "IDENTICAL" : "DIFFERS")}");
    Console.WriteLine($"  hand: {handHex}");
    Console.WriteLine($"  gen : {genHex}");
}

// -- actorJoin(28): W-1 baseline re-check (kept green) --------------------
var actorNodeRid = RoutingId.From(new byte[] { 1, 2 });
var spotNodeRid = RoutingId.From(new byte[] { 7, 8 });
var joinRequest = new ActorJoinRequest(
    Correlation: 1UL,
    Actor: new ActorRef("actor", 2UL, "default", actorNodeRid),
    ActorNodeGeneration: 3UL,
    ActorAuthorityOwnerGeneration: 4UL,
    ActorOwnerLeaseGeneration: 5UL,
    Entry: true,
    TargetSpotId: "spot",
    TargetSpotGeneration: 6UL,
    TargetNodeRid: spotNodeRid,
    TargetNodeGeneration: 9UL,
    TargetAuthorityOwnerGeneration: 10UL,
    TargetOwnerLeaseGeneration: 11UL);
var gActor = new ServiceWirePilotCodec.Fence("actor", 2UL, new byte[] { 1, 2 }, 3UL, 4UL, 5UL);
var gSpot = new ServiceWirePilotCodec.Fence("spot", 6UL, new byte[] { 7, 8 }, 9UL, 10UL, 11UL);
var gJoin = new ServiceWirePilotCodec.ActorJoin28(1UL, gActor, true, gSpot);
Check("actorJoin(28)",
    ZLinkServiceWireCodec.EncodeActorJoinRequest(joinRequest),
    ServiceWirePilotCodec.EncodeActorJoin28(gJoin));

// -- W-2 mechanical candidates ---------------------------------------------
Check("livenessProbe(5)",
    ZLinkServiceWireCodec.EncodeLiveness(ServiceWireConstants.Command.LivenessProbe, 42UL),
    ServiceWirePilotCodec.EncodeLivenessProbe5(new ServiceWirePilotCodec.LivenessProbe5(42UL)));

Check("livenessAck(6)",
    ZLinkServiceWireCodec.EncodeLiveness(ServiceWireConstants.Command.LivenessAck, 42UL),
    ServiceWirePilotCodec.EncodeLivenessAck6(new ServiceWirePilotCodec.LivenessAck6(42UL)));

Check("nodeSend(16)",
    ZLinkServiceWireCodec.EncodeApplication(ServiceWireConstants.Command.NodeSend, 0UL, null, false),
    ServiceWirePilotCodec.EncodeNodeSend16());

Check("nodeRequest(17)",
    ZLinkServiceWireCodec.EncodeApplication(ServiceWireConstants.Command.NodeRequest, 7UL, null, false),
    ServiceWirePilotCodec.EncodeNodeRequest17(new ServiceWirePilotCodec.NodeRequest17(7UL)));

Check("channelSend(18)",
    ZLinkServiceWireCodec.EncodeApplication(ServiceWireConstants.Command.ChannelSend, 0UL, "lobby", false),
    ServiceWirePilotCodec.EncodeChannelSend18(new ServiceWirePilotCodec.ChannelSend18("lobby")));

Check("channelRequest(19)",
    ZLinkServiceWireCodec.EncodeApplication(ServiceWireConstants.Command.ChannelRequest, 7UL, "lobby", false),
    ServiceWirePilotCodec.EncodeChannelRequest19(new ServiceWirePilotCodec.ChannelRequest19(7UL, "lobby")));

Check("logicalMulticast(23)",
    ZLinkServiceWireCodec.EncodeLogicalMulticast("lobby", "topicA", "spot1", false),
    ServiceWirePilotCodec.EncodeLogicalMulticast23(new ServiceWirePilotCodec.LogicalMulticast23("lobby", "topicA", "spot1")));

// actorLookup(26): no .NET hand codec exists (confirmed: no EncodeActorLookup
// in ZLinkServiceWireCodec.cs) - generated only, not equivalence-checked.
Console.WriteLine("actorLookup(26): NO HAND CODEC (new capability) - generated only");

// actorDestroy(27): .NET hand codec's ActorDestroyOperation domain type does
// NOT carry expectedOwnerLeaseGeneration (schema's actor-route-fence requires
// it) - EncodeActorDestroy omits that field entirely, so its output is 8
// bytes shorter than a schema-compliant frame. TryDecodeActorDestroy reads
// back exactly the same 4 nonzero-u64 fields (no 5th), so .NET's own
// encode/decode pair is internally consistent but NOT schema-compliant: a
// schema-compliant actorDestroy frame from Node (or the generated codec)
// has 8 trailing bytes .NET's decoder does not expect and would reject with
// DecodeError.TrailingByte. This is a genuine cross-language interop gap in
// the existing .NET hand codec, not a generator defect - out of scope to
// fix here (W-3), reported for visibility. Comparing anyway to document the
// drift precisely, rather than silently skipping it.
var actorDestroyOperation = new ActorDestroyOperation(
    Correlation: 7UL,
    Actor: new ActorRef("actor1", 2UL, "default", actorNodeRid),
    TargetNodeRid: actorNodeRid,
    TargetNodeGeneration: 3UL,
    AuthorityOwnerGeneration: 4UL);
var gActorDestroyFence = new ServiceWirePilotCodec.Fence("actor1", 2UL, new byte[] { 1, 2 }, 3UL, 4UL, 5UL);
Check("actorDestroy(27) [.NET hand codec omits expectedOwnerLeaseGeneration]",
    ZLinkServiceWireCodec.EncodeActorDestroy(actorDestroyOperation),
    ServiceWirePilotCodec.EncodeActorDestroy27(new ServiceWirePilotCodec.ActorDestroy27(7UL, gActorDestroyFence)));

Environment.Exit(allEqual ? 0 : 1);
