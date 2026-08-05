// ST-I1: Payload size profile 시나리오를 검증한다.
import {
  SpotActorTransferNames,
  actorNode,
  createActor,
  createRemoteSpot,
  getEvidence,
  joinActor,
  nodeA,
  require,
  unique,
  waitEvidence
} from '../Support/scenario-support';

const profiles = [
  { name: 'small', bytes: 4 * 1024 },
  { name: 'normal', bytes: 64 * 1024 },
  { name: 'large', bytes: 8 * 1024 * 1024 }
] as const;

export async function runStI1(): Promise<void> {
  for (const profile of profiles) {
    const actorId = unique(`actor-i1-${profile.name}`);
    const actor = await createActor(
      nodeA,
      actorId,
      SpotActorTransferNames.actorTypeStateful,
      profile.bytes,
      profile.bytes
    );
    const source = actorNode(actor.nodeRid);
    const targetSpot = await createRemoteSpot(actor.nodeRid);
    const target = actorNode(targetSpot.nodeRid);
    require((await joinActor(source, actorId, {
      scenario: 'ST-I1',
      targetSpotId: targetSpot.spotId
    })).accepted, `ST-I1 ${profile.name} relocation was rejected.`);
    await waitEvidence(target, [
      `ST-I1|${actorId}|payload_restored|application=${profile.bytes}|encoded=`
    ], 40_000);
    const sourceEvidence = await getEvidence(source);
    const captured = sourceEvidence.find(entry =>
      entry.scenario === 'ST-I1'
      && entry.actorId === actorId
      && entry.kind === 'payload_captured'
    );
    require(
      captured !== undefined
      && captured.value.startsWith(`application=${profile.bytes}|encoded=`),
      `ST-I1 ${profile.name} did not measure the encoded capture.`
    );
    const encoded = Number(captured.value.split('encoded=')[1]);
    require(
      encoded > profile.bytes,
      `ST-I1 ${profile.name} encoded payload did not include its envelope.`
    );
    console.log(
      `relocation_payload profile=${profile.name} application_bytes=${profile.bytes} encoded_bytes=${encoded}`
    );
  }
}
