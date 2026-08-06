// SF-C5: Public operational query를 bounded page로 읽는다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

interface ObjectReply {
  readonly spotId: string;
  readonly operationId: string;
  readonly payload: string;
}

interface ObjectPage {
  readonly items: readonly { readonly spotId: string }[];
  readonly continuationToken?: string;
}

export async function runSFC5(options: ClientOptions): Promise<void> {
  await postJson(options.consumerUrl, '/profile/request', { value: 'sf-c5-warmup', marker: 'SF-C5' });
  const mesh = await getJson<readonly { readonly objectRole: string; readonly state: number; readonly objectCapabilities: readonly { readonly stableType: string }[] }[]>(options.consumerUrl, '/location/mesh');
  ensure(mesh.some((row) => row.objectRole === 'server' && row.state === 1 && row.objectCapabilities.some((capability) => capability.stableType === 'Config6InstanceSpot')),
    'SF-C5 provider did not publish the Instance Spot capability.');
  const spotIds = Array.from({ length: 1001 }, (_, index) => `sf-c5-spot-${index}`);
  for (let offset = 0; offset < spotIds.length; offset += 40) {
    try {
      await Promise.all(spotIds.slice(offset, offset + 40).map(async (spotId) => {
      const operationId = `${spotId}-operation`;
      const reply = await postJson<ObjectReply>(options.consumerUrl, '/object/request', {
        spotId,
        operationId,
        payload: `payload-${spotId}`
      });
      ensure(reply.spotId === spotId, `SF-C5 object reply changed SpotId for ${spotId}.`);
      ensure(reply.operationId === operationId, `SF-C5 object reply changed operationId for ${spotId}.`);
      ensure(reply.payload === `payload-${spotId}`, `SF-C5 object payload changed for ${spotId}.`);
      }));
    } catch (error) {
      const currentMesh = await getJson<unknown>(options.consumerUrl, '/location/mesh').catch(() => 'unavailable');
      const route = await getJson<unknown>(options.consumerUrl, '/route/status').catch(() => 'unavailable');
      throw new Error(`SF-C5 object request failed: ${String(error)} mesh=${JSON.stringify(currentMesh)} route=${JSON.stringify(route)}`);
    }
  }

  for (const pageSize of [1, 100, 1000]) {
    const observed: string[] = [];
    let continuationToken: string | undefined;
    let firstPage: ObjectPage | undefined;
    for (let attempt = 0; attempt < 50; attempt += 1) {
      firstPage = await getJson<ObjectPage>(options.consumerUrl, `/location/objects?kind=spot&pageSize=${pageSize}`);
      if (firstPage.items.length > 0) break;
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    ensure(firstPage !== undefined && firstPage.items.length > 0, `SF-C5 pageSize=${pageSize} returned no rows before the query deadline.`);
    ensure(firstPage.items.length <= pageSize, `SF-C5 page exceeded pageSize=${pageSize}.`);
    observed.push(...firstPage.items.map((item) => item.spotId));
    continuationToken = firstPage.continuationToken;
    while (continuationToken !== undefined) {
      const suffix = new URLSearchParams({ kind: 'spot', pageSize: String(pageSize), continuationToken });
      const page = await getJson<ObjectPage>(options.consumerUrl, `/location/objects?${suffix}`);
      ensure(page.items.length <= pageSize, `SF-C5 page exceeded pageSize=${pageSize}.`);
      observed.push(...page.items.map((item) => item.spotId));
      continuationToken = page.continuationToken;
    }
    ensure(observed.length === spotIds.length, `SF-C5 pageSize=${pageSize} returned ${observed.length} items.`);
    ensure(new Set(observed).size === observed.length, `SF-C5 pageSize=${pageSize} returned duplicate SpotIds.`);
    ensure(spotIds.every((spotId) => observed.includes(spotId)), `SF-C5 pageSize=${pageSize} omitted a SpotId.`);
  }
  console.log('scenario SF-C5 passed');
}
