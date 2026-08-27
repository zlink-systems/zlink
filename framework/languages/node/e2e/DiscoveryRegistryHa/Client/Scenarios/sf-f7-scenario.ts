// SF-F7: Large state relocation은 chunk 경계를 넘어도 복원한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson, postJsonWithin } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

interface CapacityRes {
  readonly status: string;
}

interface ProbeRes {
  readonly actorId: string;
  readonly actorState: string;
  readonly spotId: string;
  readonly spotStateLength: number;
  readonly spotStateChecksum: string;
  readonly nodeRid: string;
}

interface LocationRes {
  readonly found: boolean;
  readonly ownerNodeRid?: string;
  readonly objectGeneration?: string;
}

export async function runSFF7(options: ClientOptions): Promise<void> {
  ensure(options.providerBUrl !== undefined, 'SF-F7 provider B URL is required.');
  const variant = options.variant;
  ensure(variant === 'boundary' || variant === 'oversize', 'SF-F7 variant is required.');
  const suffix = `${Date.now()}-${process.pid}`;
  const spotId = `sf-f7-spot-${suffix}`;
  const actorId = `sf-f7-actor-${suffix}`;
  const actorState = `actor-state-${suffix}`;
  const stateLength = 64 * 1024 * 1024 + (variant === 'oversize' ? 1 : 0);

  const spot = await postJson<CapacityRes>(options.providerAUrl, '/capacity/spots', {
    spotId,
    stateLength,
    fillByte: 0x5a
  });
  ensure(spot.status === 'created', 'SF-F7 source Spot creation failed.');
  const actor = await postJson<CapacityRes>(options.providerAUrl, '/capacity/actors', {
    actorId,
    state: actorState
  });
  ensure(actor.status === 'created', 'SF-F7 source Actor creation failed.');
  await postJson(options.providerAUrl, '/capacity/actors/join', { actorId, spotId });
  const source = await probe(options.providerAUrl, actorId);
  ensure(source.spotStateLength === stateLength, 'SF-F7 source state length changed.');
  ensure(source.actorState === actorState, 'SF-F7 source Actor state changed.');

  await postJson(options.providerAUrl, '/placement/weight', { weight: 0 });
  console.log('scenario-control SF-F7 start-provider-b');
  await waitFor(async () => {
    try {
      await getJson(options.providerBUrl!, '/health');
      return true;
    } catch {
      return false;
    }
  });

  const relocation = await postJsonWithin<{ outcome: number; reason: number }>(
    options.providerAUrl,
    '/drain',
    { deadlineMs: 180_000 },
    190_000
  );
  const expectedOwner = variant === 'boundary' ? 'api-b' : 'api-a';
  if (variant === 'boundary') {
    ensure(relocation.outcome === 0,
      `SF-F7 64 MiB state did not relocate: ${JSON.stringify(relocation)}.`);
  } else {
    ensure(relocation.outcome === 1 && relocation.reason === 4,
      `SF-F7 oversized state did not return StateIncompatible: ${JSON.stringify(relocation)}.`);
  }
  await waitFor(() => locationsOwnedBy(options, actorId, spotId, expectedOwner), 30_000);
  const restored = await probe(
    variant === 'boundary' ? options.providerBUrl : options.providerAUrl,
    actorId
  );
  ensure(restored.nodeRid === expectedOwner, 'SF-F7 request reached another owner.');
  ensure(restored.spotStateLength === stateLength, 'SF-F7 target state length changed.');
  ensure(restored.spotStateChecksum === source.spotStateChecksum, 'SF-F7 target state checksum changed.');
  ensure(restored.actorState === actorState, 'SF-F7 target Actor state changed.');
  console.log(`scenario SF-F7 variant=${variant} passed`);
}

async function probe(providerUrl: string, actorId: string): Promise<ProbeRes> {
  return await postJson(providerUrl, '/capacity/actors/probe', { actorId });
}

async function locationsOwnedBy(
  options: ClientOptions,
  actorId: string,
  spotId: string,
  ownerRid: string
): Promise<boolean> {
  const rows = await Promise.all([
    getJson<LocationRes>(options.consumerUrl,
      `/location/object?kind=actor&id=${encodeURIComponent(actorId)}`),
    getJson<LocationRes>(options.consumerUrl,
      `/location/object?kind=spot&id=${encodeURIComponent(spotId)}`)
  ]);
  return rows.every(row => row.found
    && row.ownerNodeRid === ownerRid
    && row.objectGeneration !== undefined
    && row.objectGeneration !== '0');
}

async function waitFor(
  check: () => Promise<boolean>,
  timeoutMs = 15_000
): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (await check()) return;
    await new Promise(resolve => setTimeout(resolve, 50));
  }
  throw new Error('SF-F7 timed out waiting for the public state transition.');
}
