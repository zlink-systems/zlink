// ST-F3: Bound Session cross-move order 시나리오를 검증한다.
import type { ProbeReq } from '../../Shared/messages.js';
import { SpotActorTransferNames, options, nodeA, nodeB, connectAndBind, createSpot, createActor, joinActor, waitEvidence, post, assertValuesInOrder, unique, uniqueShort, delay, require } from '../Support/scenario-support';

export async function runStF3(): Promise<void> {
  const actorId = uniqueShort('actor-handoff-gate-f3');
  const spotId = unique('spot-handoff-bound');
  await createSpot(nodeB, spotId);
  const source = await createActor(nodeA, actorId, SpotActorTransferNames.actorTypeStateful, 103);
  const connector = await connectAndBind(options.sessionAStreamEndpoint, 'ST-F3', source, uniqueShort('transfer'));
  try {
    const join = joinActor(nodeA, actorId, { scenario: 'ST-F3', targetSpotId: spotId });
    await waitEvidence(nodeA, [`ST-F3|${actorId}|before_commit_gate|103`]);
    await connector.send({ scenario: 'ST-F3', marker: 'S1' } satisfies ProbeReq)
      .packetName(SpotActorTransferNames.packetHandoff).submit();
    await connector.send({ scenario: 'ST-F3', marker: 'S2' } satisfies ProbeReq)
      .packetName(SpotActorTransferNames.packetHandoff).submit();
    // Give RouteMesh's bound-session relay time to admit both one-way packets
    // before the transfer gate lets Core snapshot the actor mailbox.
    await delay(200);
    await post(nodeA, `/transfer-gates/${actorId}/release`, {});
    await connector.send({ scenario: 'ST-F3', marker: 'S3' } satisfies ProbeReq)
      .packetName(SpotActorTransferNames.packetHandoff).submit();
    await connector.send({ scenario: 'ST-F3', marker: 'S4' } satisfies ProbeReq)
      .packetName(SpotActorTransferNames.packetHandoff).submit();
    require((await join).accepted, 'ST-F3 join failed.');
    const entries = await waitEvidence(nodeB, ['S1', 'S2', 'S3', 'S4'].map(
      (marker) => `ST-F3|${actorId}|packet_handler|${marker}`
    ));
    assertValuesInOrder(entries, actorId, 'packet_handler', ['S1', 'S2', 'S3', 'S4']);
  } finally {
    await connector.close();
  }
}
