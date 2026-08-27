// SF-F11: Waiter 종료와 전송 실패 뒤 payload 값을 보존한다 시나리오를 검증한다.
import { postJson } from '../../../http-client';
import type { ClientOptions } from '../Support/client-options';
import { ensure } from '../Support/scenario-assert';
import {
  createAggregate,
  expectAggregate,
  objectLocation,
  relocate,
  setPlacementWeight,
  setScenarioGate,
  waitFor
} from '../Support/relocation-fixture';

const first = {
  actorId: 'sf-f11-actor-a',
  spotId: 'sf-f11-spot-a',
  actorGeneration: '',
  state: 'payload-a-7d8ec8f4'
};
const second = {
  actorId: 'sf-f11-actor-b',
  spotId: 'sf-f11-spot-b',
  actorGeneration: '',
  state: 'payload-b-13f36a09'
};

export async function runSFF11(options: ClientOptions): Promise<void> {
  ensure(options.providerBUrl !== undefined, 'SF-F11 provider B URL is required.');
  const phase = options.phase ?? 'setup';
  if (phase === 'setup') {
    await createAggregate(options.providerAUrl, first.actorId, first.spotId, first.state);
    await setScenarioGate(options.providerAUrl, 'capture', true);
    await setPlacementWeight(options.providerAUrl, 0);
    console.log('scenario SF-F11 setup passed');
    return;
  }
  if (phase === 'failure') {
    const result = await relocate(options.providerAUrl, 20_000);
    ensure(result.outcome !== 0, 'SF-F11 response-loss operation unexpectedly succeeded.');
    await waitFor(async () => {
      const actor = await objectLocation(options, 'actor', first.actorId);
      const spot = await objectLocation(options, 'spot', first.spotId);
      return actor.found && actor.ownerNodeRid === 'api-a'
        && spot.found && spot.ownerNodeRid === 'api-a';
    }, 'SF-F11 source authority after response loss');
    await expectAggregate(options.providerAUrl, first, 'api-a');
    console.log('scenario SF-F11 response-loss passed');
    return;
  }
  if (phase === 'prepare-b') {
    await setPlacementWeight(options.providerAUrl, 100);
    const left = await postJson<{ accepted: boolean }>(
      options.providerAUrl,
      '/capacity/actors/leave',
      { actorId: first.actorId }
    );
    ensure(left.accepted,
      'SF-F11 failed to remove payload A Actor membership.');
    let removed = false;
    await waitFor(async () => {
      try {
        removed = (await postJson<{ destroyed: boolean }>(
          options.providerAUrl,
          '/capacity/actors/destroy',
          { actorId: first.actorId }
        )).destroyed;
        return removed;
      } catch {
        return false;
      }
    }, 'SF-F11 payload A Actor removal');
    ensure(removed, 'SF-F11 failed to remove payload A Actor.');
    let closed = false;
    await waitFor(async () => {
      closed = (await postJson<{ closed: boolean }>(
        options.providerAUrl,
        '/capacity/spots/close',
        { spotId: first.spotId }
      )).closed;
      return closed;
    }, 'SF-F11 payload A User Spot removal');
    ensure(closed, 'SF-F11 failed to remove payload A User Spot.');
    await createAggregate(options.providerAUrl, second.actorId, second.spotId, second.state);
    await setPlacementWeight(options.providerAUrl, 0);
    console.log('scenario SF-F11 payload-b prepared');
    return;
  }
  ensure(phase === 'recovery', `Unsupported SF-F11 phase '${phase}'.`);
  const result = await relocate(options.providerAUrl, 60_000);
  ensure(result.outcome === 0, 'SF-F11 payload B relocation did not succeed.');
  await waitFor(
    async () => (await objectLocation(options, 'actor', second.actorId)).ownerNodeRid === 'api-b',
    'SF-F11 payload B target ownership'
  );
  await expectAggregate(options.providerBUrl, second, 'api-b');
  ensure(!(await objectLocation(options, 'actor', first.actorId)).found,
    'SF-F11 payload A Actor reappeared after payload B relocation.');
  console.log('scenario SF-F11 passed');
}
