// SF-F6: Operational query 중 concurrent 변경을 다음 page cycle에 반영한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

interface ObjectReply { readonly spotId: string; readonly operationId: string; readonly payload: string; }
interface ObjectPage { readonly items: readonly { readonly spotId: string }[]; readonly continuationToken?: string; }

export async function runSFF6(options: ClientOptions): Promise<void> {
  const spotIds = Array.from({ length: 1001 }, (_, index) => `sf-f6-spot-${index}`);
  for (let offset = 0; offset < spotIds.length; offset += 40) {
    await Promise.all(spotIds.slice(offset, offset + 40).map((spotId) => request(options, spotId, `${spotId}-operation`)));
  }
  const first = await getJson<ObjectPage>(options.consumerUrl, '/location/objects?kind=spot&pageSize=100');
  ensure(first.items.length === 100, 'SF-F6 first page did not respect pageSize=100.');
  const removed = first.items[0]?.spotId;
  ensure(removed !== undefined, 'SF-F6 first page was empty.');
  const added = 'sf-f6-added';
  await request(options, added, `${added}-operation`);
  await request(options, removed, '__close__');

  const observed: string[] = first.items.map((item) => item.spotId);
  let continuationToken = first.continuationToken;
  while (continuationToken !== undefined) {
    const page = await getJson<ObjectPage>(
      options.consumerUrl,
      `/location/objects?kind=spot&pageSize=100&continuationToken=${encodeURIComponent(continuationToken)}`
    );
    ensure(page.items.length <= 100, 'SF-F6 continuation page exceeded pageSize=100.');
    observed.push(...page.items.map((item) => item.spotId));
    continuationToken = page.continuationToken;
  }
  ensure(new Set(observed).size === observed.length, 'SF-F6 scan returned duplicate SpotIds.');
  ensure(
    observed.every((spotId) => spotIds.includes(spotId) || spotId === added),
    'SF-F6 completed scan returned an unexpected SpotId.'
  );

  const second = await getJson<ObjectPage>(options.consumerUrl, '/location/objects?kind=spot&pageSize=1000');
  const secondIds = new Set(second.items.map((item) => item.spotId));
  let secondContinuationToken = second.continuationToken;
  while (secondContinuationToken !== undefined) {
    const page = await getJson<ObjectPage>(
      options.consumerUrl,
      `/location/objects?kind=spot&pageSize=1000&continuationToken=${encodeURIComponent(secondContinuationToken)}`
    );
    ensure(page.items.length <= 1000, 'SF-F6 large continuation page exceeded pageSize=1000.');
    page.items.forEach((item) => {
      ensure(!secondIds.has(item.spotId), 'SF-F6 large scan returned duplicate SpotIds.');
      secondIds.add(item.spotId);
    });
    secondContinuationToken = page.continuationToken;
  }
  ensure(secondIds.has(added), 'SF-F6 second scan did not reflect the created object.');
  ensure(!secondIds.has(removed), 'SF-F6 second scan did not reflect the closed object.');
  ensure(secondIds.size === spotIds.length, 'SF-F6 second scan returned an unexpected object count.');
  console.log('scenario SF-F6 passed');
}

async function request(options: ClientOptions, spotId: string, operationId: string): Promise<ObjectReply> {
  return await postJson<ObjectReply>(options.consumerUrl, '/object/request', {
    spotId, operationId, payload: `payload-${spotId}`
  });
}
