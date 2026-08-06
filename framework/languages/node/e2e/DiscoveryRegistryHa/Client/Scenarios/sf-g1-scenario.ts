// SF-G1: Actor·Spot·stable type limit을 atomic하게 적용한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

interface MeshRow {
  readonly rid: string;
  readonly populationCapacity: {
    readonly actors: { readonly limit: number; readonly active: number };
    readonly spots: { readonly limit: number; readonly active: number };
    readonly spotTypes: readonly { readonly stableType: string; readonly limit: number; readonly active: number }[];
  };
}

interface ObjectReply { readonly spotId: string; readonly payload: string; }
interface ObjectPage { readonly items: readonly { readonly spotId: string }[]; }

export async function runSFG1(options: ClientOptions): Promise<void> {
  const mesh = await getJson<readonly MeshRow[]>(options.consumerUrl, '/location/mesh');
  const provider = mesh.find((row) => row.rid === 'api-a');
  ensure(provider !== undefined, 'SF-G1 provider descriptor is missing.');
  ensure(provider.populationCapacity.spots.limit === 4, 'SF-G1 Spot limit is not 4.');
  ensure(provider.populationCapacity.spotTypes.some((row) => row.stableType === 'Config6InstanceSpot' && row.limit === 2),
    'SF-G1 stable type limit is not 2.');
  const results = await Promise.allSettled(Array.from({ length: 8 }, (_, index) => {
    const spotId = `sf-g1-spot-${index}`;
    return postJson<ObjectReply>(options.consumerUrl, '/object/request', {
      spotId, operationId: `${spotId}-operation`, payload: `payload-${spotId}`
    });
  }));
  const successful = results.filter((result): result is PromiseFulfilledResult<ObjectReply> => result.status === 'fulfilled');
  ensure(successful.length === 2, `SF-G1 expected exactly 2 stable-type admissions, got ${successful.length}.`);
  ensure(new Set(successful.map((result) => result.value.spotId)).size === successful.length,
    'SF-G1 admitted duplicate SpotIds.');
  let current: MeshRow | undefined;
  let admittedObjects: readonly { readonly spotId: string }[] = [];
  const deadline = Date.now() + 3000;
  while (Date.now() < deadline) {
    const after = await getJson<readonly MeshRow[]>(options.consumerUrl, '/location/mesh');
    current = after.find((row) => row.rid === 'api-a');
    const page = await getJson<ObjectPage>(options.consumerUrl, '/location/objects?kind=instance_spot&pageSize=10');
    admittedObjects = page.items;
    if (admittedObjects.length === 2) break;
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  ensure(current !== undefined, 'SF-G1 provider descriptor disappeared.');
  ensure(admittedObjects.length === 2, 'SF-G1 public object query did not retain exactly two admitted objects.');
  ensure(current.populationCapacity.spots.active <= 4, 'SF-G1 Spot active count exceeded its limit.');
  ensure(current.populationCapacity.spotTypes.every((row) => row.active <= row.limit),
    'SF-G1 stable type active count exceeded its limit.');
  console.log('scenario SF-G1 passed');
}
