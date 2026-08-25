// SF-F2: 장기 relocation은 operation deadline 안에서 완료하고 실패 뒤 새 call을 허용한다 시나리오를 검증한다.
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

export async function runSFF2(options: ClientOptions): Promise<void> {
  ensure(options.providerBUrl !== undefined, 'SF-F2 provider B URL is required.');
  const variant = options.variant ?? 'long';
  const phase = options.phase ?? 'setup';
  const identity = {
    actorId: `sf-f2-${variant}-actor`,
    spotId: `sf-f2-${variant}-spot`,
    actorGeneration: '',
    state: `sf-f2-${variant}-state`
  };
  if (phase === 'setup') {
    await createAggregate(
      options.providerAUrl,
      identity.actorId,
      identity.spotId,
      identity.state
    );
    if (variant === 'long') await setScenarioGate(options.providerAUrl, 'capture', true);
    await setPlacementWeight(options.providerAUrl, 0);
    console.log(`scenario SF-F2 variant=${variant} setup passed`);
    return;
  }
  if (phase === 'failure') {
    const result = await relocate(options.providerAUrl, 15_000);
    ensure(result.outcome !== 0, 'SF-F2 Store-fault operation unexpectedly succeeded.');
    await expectAggregate(options.providerAUrl, identity, 'api-a');
    console.log('scenario SF-F2 fault terminal passed');
    return;
  }
  ensure(phase === 'relocate' || phase === 'recovery', `Unsupported SF-F2 phase '${phase}'.`);
  const result = await relocate(options.providerAUrl, 60_000);
  ensure(result.outcome === 0, `SF-F2 ${phase} relocation did not succeed.`);
  await waitFor(
    async () => (await objectLocation(options, 'actor', identity.actorId)).ownerNodeRid === 'api-b',
    `SF-F2 ${phase} target ownership`
  );
  await expectAggregate(options.providerBUrl, identity, 'api-b');
  console.log(`scenario SF-F2 variant=${variant} ${phase} passed`);
}
