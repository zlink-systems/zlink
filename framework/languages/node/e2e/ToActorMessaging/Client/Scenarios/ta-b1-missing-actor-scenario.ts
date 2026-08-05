// TA-B1: 존재하지 않는 Actor를 호출한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson } from '../../../http-client';
import {
  type ActorEvidence, assertFailure, requireNoEvidence
} from '../Support/actor-scenario-support';

export async function runTaB1(options: ClientOptions): Promise<void> {
  await assertFailure(options, 'TA-B1-missing-send', 'ta-b1-missing', 'NotFound', true);
  await assertFailure(options, 'TA-B1-missing-request', 'ta-b1-missing', 'NotFound', false);
  requireNoEvidence(await getJson<ActorEvidence[]>(`${options.actorUrl}/evidence`), 'TA-B1-missing-send');
  console.log('scenario TA-B1 passed');
}
