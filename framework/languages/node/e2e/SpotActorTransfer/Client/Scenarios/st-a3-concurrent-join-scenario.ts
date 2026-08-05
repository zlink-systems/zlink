// ST-A3: Local Join callback boundary 시나리오를 검증한다.
import type { GateReleaseRes } from '../../Shared/messages.js';
import { SpotActorTransferNames, nodeA, createSpot, createActor, joinActor, probeActor, waitEvidence, post, has, unique, delay, isPending, require } from '../Support/scenario-support';

export async function runStA3(): Promise<void> {
  const actorId = unique('actor-local-moving');
  const spotId = unique('spot-local-moving');
  await createSpot(nodeA, spotId, 'delay-joined');
  await createActor(nodeA, actorId, SpotActorTransferNames.actorTypeStateful, 13);
  const joinPromise = joinActor(nodeA, actorId, { scenario: 'ST-A3', targetSpotId: spotId });
  const waiting = await waitEvidence(nodeA, [
    `ST-A3|${actorId}|admission|spot=${spotId}`,
    `ST-A3|${actorId}|joined_wait|${spotId}`
  ]);
  require(!has(waiting, actorId, 'packet_handler'), 'ST-A3 packet ran before joined gate release.');
  const probePromise = probeActor(nodeA, actorId, 'ST-A3', 'during-joined-wait');
  await delay(300);
  require(await isPending(probePromise), 'ST-A3 packet completed while actor was moving.');
  const gate = await post<GateReleaseRes>(nodeA, `/joined-gates/${spotId}/release`, {});
  require(gate.released, 'ST-A3 joined gate was already released.');
  require((await joinPromise).accepted, 'ST-A3 join failed.');
  const probe = await probePromise;
  require(probe.spotId === spotId, 'ST-A3 queued packet did not resume on target.');
}
