// SF-F3: Relocation Store 장애가 source workload와 Location Store를 변경하지 않는지 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { ensure } from '../Support/scenario-assert';
import {
  createAggregate,
  expectAggregate,
  objectLocation,
  relocate,
  setPlacementWeight,
  waitFor
} from '../Support/relocation-fixture';

const actorId = 'sf-f3-actor';
const spotId = 'sf-f3-spot';
const state = 'sf-f3-state';

export async function runSFF3(options: ClientOptions): Promise<void> {
  ensure(options.providerBUrl !== undefined, 'SF-F3 provider B URL is required.');
  const phase = process.env.SF_F3_PHASE ?? 'setup';
  const identity = { actorId, spotId, state, actorGeneration: '' };
  if (phase === 'setup') {
    await createAggregate(options.providerAUrl, actorId, spotId, state);
    await setPlacementWeight(options.providerAUrl, 0);
    console.log('scenario SF-F3 setup passed');
    return;
  }
  if (phase === 'failure') {
    const result = await relocate(options.providerAUrl, 15_000);
    ensure(result.outcome !== 0, 'SF-F3 relocation unexpectedly succeeded while Relocation Store was unavailable.');
    await expectAggregate(options.providerAUrl, identity, 'api-a');
    ensure((await objectLocation(options, 'actor', actorId)).ownerNodeRid === 'api-a',
      'SF-F3 failed relocation changed the public source owner.');
    console.log('scenario SF-F3 failure passed');
    return;
  }
  ensure(phase === 'recovery', `Unsupported SF-F3 phase '${phase}'.`);
  const result = await relocate(options.providerAUrl);
  ensure(result.outcome === 0, 'SF-F3 fresh relocation did not succeed after Store recovery.');
  await waitFor(
    async () => (await objectLocation(options, 'actor', actorId)).ownerNodeRid === 'api-b',
    'SF-F3 recovered target ownership'
  );
  await expectAggregate(options.providerBUrl, identity, 'api-b');
  console.log('scenario SF-F3 passed');
}
