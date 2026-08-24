const path = require("path");
const fs = require("fs");
const framework = "/home/hep7/project/zlink/framework/languages/node/packages/framework/dist";
const { encodeActorJoinHeader } = require(path.join(framework, "runtime/foundation/service-stateful-wire-codec.js"));
const { decodeRoutingId } = require(path.join(framework, "runtime/routing-id.js"));

async function main() {
  const { encodeActorJoin28 } = await import(
    "/home/hep7/project/zlink/framework/runtime/protocol/generated/node/service_wire_pilot_codec.generated.ts"
  );

  const fixture = JSON.parse(fs.readFileSync(
    "/home/hep7/project/zlink/framework/runtime/protocol/generated/fixtures/actor-join-28-pilot.json", "utf8"));
  const input = fixture.input;

  const actorNodeRid = decodeRoutingId("", input.actor.targetNodeRidHex);
  const spotNodeRid = decodeRoutingId("", input.targetSpot.targetNodeRidHex);

  const handHeader = encodeActorJoinHeader(
    BigInt(input.correlation),
    {
      actor: { actorId: input.actor.id, generation: BigInt(input.actor.generation), nodeRid: actorNodeRid },
      targetNodeGeneration: BigInt(input.actor.targetNodeGeneration),
      authorityOwnerGeneration: BigInt(input.actor.expectedAuthorityOwnerGeneration),
      ownerLeaseGeneration: BigInt(input.actor.expectedOwnerLeaseGeneration),
    },
    input.entry,
    {
      spot: { spotId: input.targetSpot.id, generation: BigInt(input.targetSpot.generation) },
      targetNodeRid: spotNodeRid,
      targetNodeGeneration: BigInt(input.targetSpot.targetNodeGeneration),
      authorityOwnerGeneration: BigInt(input.targetSpot.expectedAuthorityOwnerGeneration),
      ownerLeaseGeneration: BigInt(input.targetSpot.expectedOwnerLeaseGeneration),
    },
  );

  const generatedBytes = encodeActorJoin28({
    correlation: BigInt(input.correlation),
    actor: {
      id: input.actor.id,
      generation: BigInt(input.actor.generation),
      targetNodeRid: Uint8Array.from(Buffer.from(input.actor.targetNodeRidHex, "hex")),
      targetNodeGeneration: BigInt(input.actor.targetNodeGeneration),
      expectedAuthorityOwnerGeneration: BigInt(input.actor.expectedAuthorityOwnerGeneration),
      expectedOwnerLeaseGeneration: BigInt(input.actor.expectedOwnerLeaseGeneration),
    },
    entry: input.entry,
    targetSpot: {
      id: input.targetSpot.id,
      generation: BigInt(input.targetSpot.generation),
      targetNodeRid: Uint8Array.from(Buffer.from(input.targetSpot.targetNodeRidHex, "hex")),
      targetNodeGeneration: BigInt(input.targetSpot.targetNodeGeneration),
      expectedAuthorityOwnerGeneration: BigInt(input.targetSpot.expectedAuthorityOwnerGeneration),
      expectedOwnerLeaseGeneration: BigInt(input.targetSpot.expectedOwnerLeaseGeneration),
    },
  });

  const handHex = Buffer.from(handHeader).toString("hex");
  const genHex = Buffer.from(generatedBytes).toString("hex");
  const fixtureHex = fixture.hex;

  console.log("hand codec :", handHex);
  console.log("generated  :", genHex);
  console.log("fixture hex:", fixtureHex);
  console.log("hand == generated:", handHex === genHex);
  console.log("hand == fixture  :", handHex === fixtureHex);
  console.log("generated == fixture:", genHex === fixtureHex);
}
main();
