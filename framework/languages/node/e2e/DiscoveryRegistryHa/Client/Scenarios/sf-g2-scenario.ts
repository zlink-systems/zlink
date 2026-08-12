// SF-G2: Unlimited population과 activation concurrency를 구분한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

interface MeshRow {
  readonly rid: string;
  readonly populationCapacity: {
    readonly actors: { readonly limit: number };
    readonly spots: { readonly limit: number };
    readonly spotTypes: readonly { readonly stableType: string; readonly limit: number }[];
  };
  readonly activationConcurrency: { readonly active: number; readonly limit: number };
}

interface ObjectRes { readonly spotId: string; readonly operationId: string; readonly payload: string; }

export async function runSFG2(options: ClientOptions): Promise<void> {
  const mesh = await getJson<readonly MeshRow[]>(options.consumerUrl, '/location/mesh');
  const provider = mesh.find((row) => row.rid === 'api-a');
  ensure(provider !== undefined, 'SF-G2 provider descriptor is missing.');
  ensure(provider.populationCapacity.actors.limit === 0, 'SF-G2 Actor population limit is not unlimited.');
  ensure(provider.populationCapacity.spots.limit === 0, 'SF-G2 Spot population limit is not unlimited.');
  ensure(provider.populationCapacity.spotTypes.some((row) => row.stableType === 'Config6InstanceSpot' && row.limit === 0),
    'SF-G2 stable type limit is not unlimited.');
  ensure(provider.activationConcurrency.limit === 2, 'SF-G2 activation concurrency limit is not 2.');

  let complete = false;
  let maximumActive = 0;
  const requestsPromise = Promise.all(Array.from({ length: 32 }, (_, index) => {
    const spotId = `sf-g2-spot-${index}`;
    return postJson<ObjectRes>(options.consumerUrl, '/object/request', {
      spotId, operationId: `${spotId}-operation`, payload: `payload-${spotId}`
    });
  })).finally(() => { complete = true; });
  while (!complete) {
    const current = await getJson<readonly MeshRow[]>(options.consumerUrl, '/location/mesh');
    const row = current.find((entry) => entry.rid === 'api-a');
    maximumActive = Math.max(maximumActive, row?.activationConcurrency.active ?? 0);
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
  const requests = await requestsPromise;
  ensure(requests.length === 32, 'SF-G2 did not complete every valid create.');
  ensure(new Set(requests.map((reply) => reply.spotId)).size === 32, 'SF-G2 returned duplicate SpotIds.');
  ensure(requests.every((reply) => reply.payload.startsWith('payload-sf-g2-')), 'SF-G2 payload mismatch.');
  ensure(maximumActive <= 2,
    `SF-G2 activation concurrency exceeded its public limit: ${maximumActive}.`);
  ensure(maximumActive > 0, 'SF-G2 did not observe an active factory in the public descriptor.');
  console.log('scenario SF-G2 passed');
}
