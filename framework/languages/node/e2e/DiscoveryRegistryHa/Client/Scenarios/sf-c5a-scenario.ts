// SF-C5A: ID 조회와 page 결과의 object 상태를 구분한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJsonWithin } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

type ObjectState = 'creating' | 'ready' | 'unavailable';

interface ObjectLookup {
  readonly found: boolean;
  readonly objectId?: string;
  readonly state?: ObjectState;
}

interface ObjectPage {
  readonly items: readonly { readonly spotId: string; readonly state: ObjectState }[];
  readonly continuationToken?: string;
}

export async function runSFC5A(options: ClientOptions): Promise<void> {
  const missingId = 'sf-c5a-missing';
  const unavailableId = 'sf-c5a-unavailable';
  const readyId = 'sf-c5a-ready';
  const creatingId = 'sf-c5a-creating';

  ensure(!(await lookup(options, missingId)).found, 'SF-C5A missing exact lookup returned an entry.');
  await createObject(options, unavailableId);
  ensure((await lookup(options, unavailableId)).state === 'ready', 'SF-C5A baseline object is not Ready.');

  console.log('scenario-control SF-C5A kill-provider-a');
  await waitForState(options, unavailableId, 'unavailable');
  console.log('scenario-control SF-C5A start-provider-b');
  await waitForProviderReady(options);

  await createObject(options, readyId);
  const creating = createObject(options, creatingId);
  await waitForState(options, creatingId, 'creating');

  const exact = await Promise.all([
    lookup(options, missingId),
    lookup(options, unavailableId),
    lookup(options, readyId),
    lookup(options, creatingId)
  ]);
  ensure(!exact[0].found, 'SF-C5A missing exact lookup changed during the scenario.');
  ensure(exact[1].state === 'unavailable', 'SF-C5A exact lookup did not preserve Unavailable.');
  ensure(exact[2].state === 'ready', 'SF-C5A exact lookup did not preserve Ready.');
  ensure(exact[3].state === 'creating', 'SF-C5A exact lookup did not preserve Creating.');

  const page = await readAllObjects(options);
  ensure(!page.has(missingId), 'SF-C5A page included the missing object.');
  ensure(page.get(unavailableId) === 'unavailable', 'SF-C5A page disagreed with Unavailable exact lookup.');
  ensure(page.get(readyId) === 'ready', 'SF-C5A page disagreed with Ready exact lookup.');
  ensure(page.get(creatingId) === 'creating', 'SF-C5A page disagreed with Creating exact lookup.');
  await creating;

  console.log('scenario-control SF-C5A stop-redis');
  await waitForPageFailure(options);
  console.log('scenario SF-C5A passed');
}

async function createObject(options: ClientOptions, spotId: string): Promise<void> {
  const deadline = Date.now() + 30_000;
  let lastError: unknown;
  while (Date.now() < deadline) {
    try {
      const reply = await postJsonWithin<{ readonly spotId: string }>(
        options.consumerUrl,
        '/object/request',
        { spotId, operationId: `${spotId}-operation`, payload: spotId },
        10_000
      );
      ensure(reply.spotId === spotId, `SF-C5A object reply changed SpotId for ${spotId}.`);
      return;
    } catch (error) {
      lastError = error;
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
  }
  throw new Error(`SF-C5A object ${spotId} was not admitted: ${String(lastError)}`);
}

async function lookup(options: ClientOptions, spotId: string): Promise<ObjectLookup> {
  const query = new URLSearchParams({ kind: 'spot', id: spotId });
  return await getJson<ObjectLookup>(options.consumerUrl, `/location/object?${query}`);
}

async function waitForState(
  options: ClientOptions,
  spotId: string,
  expected: ObjectState
): Promise<void> {
  const deadline = Date.now() + 15_000;
  let last: ObjectLookup | undefined;
  while (Date.now() < deadline) {
    last = await lookup(options, spotId);
    if (last.state === expected) return;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`SF-C5A ${spotId} did not become ${expected}; last=${JSON.stringify(last)}`);
}

async function waitForProviderReady(options: ClientOptions): Promise<void> {
  const providerUrl = options.providerBUrl;
  ensure(providerUrl !== undefined, 'SF-C5A provider B URL is required.');
  const deadline = Date.now() + 15_000;
  while (Date.now() < deadline) {
    try {
      const health = await getJson<{ readonly status: string }>(providerUrl, '/health');
      const route = await getJson<{
        readonly isReady: boolean;
      }>(options.consumerUrl, '/route/status');
      if (health.status === 'ready' && route.isReady) return;
    } catch {
      // The runner starts provider B after it observes the control marker.
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error('SF-C5A provider B did not become ready.');
}

async function readAllObjects(options: ClientOptions): Promise<Map<string, ObjectState>> {
  const rows = new Map<string, ObjectState>();
  let continuationToken: string | undefined;
  do {
    const query = new URLSearchParams({ kind: 'spot', pageSize: '1000' });
    if (continuationToken !== undefined) query.set('continuationToken', continuationToken);
    const page = await getJson<ObjectPage>(options.consumerUrl, `/location/objects?${query}`);
    for (const item of page.items) rows.set(item.spotId, item.state);
    continuationToken = page.continuationToken;
  } while (continuationToken !== undefined);
  return rows;
}

async function waitForPageFailure(options: ClientOptions): Promise<void> {
  const deadline = Date.now() + 15_000;
  while (Date.now() < deadline) {
    try {
      await getJson<ObjectPage>(options.consumerUrl, '/location/objects?kind=spot&pageSize=1000');
    } catch {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error('SF-C5A Store failure returned a partial successful page.');
}
