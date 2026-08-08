import { getJson, postJson, postJsonWithin } from '../../../http-client';
import type { ClientOptions } from './client-options';
import { ensure } from './scenario-assert';

export interface ObjectLocation {
  readonly found: boolean;
  readonly ownerNodeRid?: string;
  readonly objectGeneration?: string;
}

export interface AggregateIdentity {
  readonly actorId: string;
  readonly spotId: string;
  readonly actorGeneration: string;
  readonly state: string;
}

export interface AggregateProbe {
  readonly actorId: string;
  readonly actorState: string;
  readonly spotId: string;
  readonly spotState: string;
  readonly nodeRid: string;
}

export async function createAggregate(
  providerUrl: string,
  actorId: string,
  spotId: string,
  state: string
): Promise<AggregateIdentity> {
  const actor = await postJson<{ status: string; objectGeneration: string }>(
    providerUrl,
    '/capacity/actors',
    { actorId, state }
  );
  ensure(actor.status === 'created', `Actor '${actorId}' was not created.`);
  const spot = await postJson<{ status: string }>(providerUrl, '/capacity/spots', { spotId, state });
  ensure(spot.status !== 'error', `User Spot '${spotId}' was not created.`);
  await postJson(providerUrl, '/capacity/actors/join', { actorId, spotId });
  return { actorId, spotId, actorGeneration: actor.objectGeneration, state };
}

export async function probeAggregate(
  providerUrl: string,
  identity: AggregateIdentity
): Promise<AggregateProbe> {
  return await postJson<AggregateProbe>(providerUrl, '/capacity/actors/probe', {
    actorId: identity.actorId
  });
}

export async function expectAggregate(
  providerUrl: string,
  identity: AggregateIdentity,
  ownerRid: string
): Promise<void> {
  let probe: AggregateProbe | undefined;
  await waitFor(async () => {
    try {
      probe = await probeAggregate(providerUrl, identity);
      return true;
    } catch {
      return false;
    }
  }, `aggregate '${identity.spotId}' request readiness`);
  ensure(probe !== undefined, `Aggregate '${identity.spotId}' did not return a probe.`);
  ensure(probe.actorState === identity.state, `Actor '${identity.actorId}' state changed.`);
  ensure(probe.spotState === identity.state, `User Spot '${identity.spotId}' state changed.`);
  ensure(probe.nodeRid === ownerRid, `Aggregate '${identity.spotId}' is not served by ${ownerRid}.`);
}

export async function setScenarioGate(
  providerUrl: string,
  name: 'capture' | 'restore' | 'request' | 'initialize',
  closed: boolean
): Promise<void> {
  await postJson(providerUrl, `/scenario-gate/${closed ? 'close' : 'open'}`, { name });
}

export async function setPlacementWeight(providerUrl: string, weight: number): Promise<void> {
  await postJson(providerUrl, '/placement/weight', { weight });
}

export async function relocate(
  providerUrl: string,
  deadlineMs = 60_000
): Promise<{ readonly outcome: number; readonly reason: number }> {
  return await postJsonWithin(providerUrl, '/drain', { deadlineMs, stopOnSuccess: false }, deadlineMs + 10_000);
}

export async function objectLocation(
  options: ClientOptions,
  kind: 'actor' | 'spot',
  id: string
): Promise<ObjectLocation> {
  return await getJson<ObjectLocation>(
    options.consumerUrl,
    `/location/object?kind=${kind}&id=${encodeURIComponent(id)}`
  );
}

export async function waitFor(
  predicate: () => Promise<boolean>,
  description: string,
  timeoutMs = 30_000
): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (await predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`Timed out waiting for ${description}.`);
}
