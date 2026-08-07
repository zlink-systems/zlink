// SF-F8: restore 중 target owner lease가 만료되면 source authority를 유지하는지 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { postJsonWithin } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';
import {
  createAggregate,
  expectAggregate,
  objectLocation,
  relocate,
  setPlacementWeight
} from '../Support/relocation-fixture';

const identity = {
  actorId: 'sf-f8-actor',
  spotId: 'sf-f8-spot',
  actorGeneration: '',
  state: 'sf-f8-state'
};

export async function runSFF8(options: ClientOptions): Promise<void> {
  const phase = process.env.SF_F8_PHASE ?? 'setup';
  if (phase === 'setup') {
    await createAggregate(
      options.providerAUrl,
      identity.actorId,
      identity.spotId,
      identity.state
    );
    await setPlacementWeight(options.providerAUrl, 0);
    console.log('scenario SF-F8 setup passed');
    return;
  }
  ensure(phase === 'relocate', `Unsupported SF-F8 phase '${phase}'.`);
  const result = await relocate(options.providerAUrl, 30_000);
  ensure(result.outcome !== 0, 'SF-F8 relocation committed through an expired target owner lease.');
  await expectAggregate(options.providerAUrl, identity, 'api-a');
  ensure((await objectLocation(options, 'actor', identity.actorId)).ownerNodeRid === 'api-a',
    'SF-F8 failed relocation changed the public source owner.');
  const followUp = await postJsonWithin<{
    readonly operationId: string;
    readonly providerRid: string;
  }>(options.consumerUrl, '/object/request', {
    spotId: identity.spotId,
    operationId: 'sf-f8-follow-up',
    payload: 'sf-f8-follow-up',
    instanceSpot: false
  }, 15_000);
  ensure(followUp.operationId === 'sf-f8-follow-up', 'SF-F8 source follow-up terminal belonged to another operation.');
  ensure(followUp.providerRid === 'api-a', 'SF-F8 source follow-up was not handled by api-a.');
  console.log('scenario SF-F8 passed');
}
