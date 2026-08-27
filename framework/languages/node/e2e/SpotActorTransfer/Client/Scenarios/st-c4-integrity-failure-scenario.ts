// ST-C4: 직접 전송 integrity failure 시나리오를 검증한다.
import {
  SpotActorTransferNames,
  createActor,
  createSpot,
  getEvidence,
  joinActor,
  nodeA,
  nodeB,
  probeActor,
  require,
  unique,
  waitEvidence
} from '../Support/scenario-support';

type Variant = 'checksum-mismatch' | 'identity-conflict';

interface FaultState {
  readonly fired: Partial<Record<Variant, number>>;
}

export async function runStC4(): Promise<void> {
  for (const variant of ['checksum-mismatch', 'identity-conflict'] as const) {
    await verifyVariant(variant);
  }
}

async function verifyVariant(variant: Variant): Promise<void> {
  const actorId = unique(`actor-c4-${variant}`);
  const spotId = unique(`spot-c4-${variant}`);
  await createSpot(nodeB, spotId);
  await createActor(nodeA, actorId, SpotActorTransferNames.actorTypeStateful, 84);
  await post(`/relocation-integrity/${variant}/arm`, {});
  const join = await joinActor(nodeA, actorId, { scenario: 'ST-C4', targetSpotId: spotId });
  require(join.accepted, `ST-C4 ${variant} Join was not accepted for deferred completion.`);

  const source = await waitEvidence(nodeA, [`ST-C4|${actorId}|join_completion|failed|`]);
  const terminals = source.filter(entry => entry.scenario === 'ST-C4'
    && entry.actorId === actorId && entry.kind === 'join_completion');
  require(terminals.length === 1 && terminals[0]!.value.startsWith('failed|'), `ST-C4 ${variant} did not produce exactly one failure terminal.`);
  const fault = await nodeB.get('/relocation-integrity').fetch<FaultState>();
  require(fault.fired[variant] === 1, `ST-C4 ${variant} fault was not injected exactly once.`);

  const sourceProbe = await probeActor(nodeA, actorId, 'ST-C4', `source-survives-${variant}`);
  require(sourceProbe.nodeRid === 'actor-a' && sourceProbe.stateVersion === 84, `ST-C4 ${variant} source Actor did not retain its original state.`);

  const target = await getEvidence(nodeB);
  require(!target.some(entry => entry.actorId === actorId && (entry.kind === 'transfer_in' || entry.kind === 'joined')), `ST-C4 ${variant} restored or joined a partial target payload.`);
  require(!source.some(entry => entry.actorId === actorId && (entry.kind === 'leave' || entry.kind === 'source_cleanup')), `ST-C4 ${variant} removed the source Actor after target failure.`);
}

async function post<T>(path: string, body: unknown): Promise<T> {
  return await nodeB.post(path).body(body).fetch<T>();
}
